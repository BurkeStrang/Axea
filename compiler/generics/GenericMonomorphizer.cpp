#include "generics/GenericMonomorphizer.hpp"

#include "ast/Expr.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // Every built-in generic-shaped type name already hardcoded in
    // Parser::parseTypeNameAtom/TypeChecker::resolveType - never StructDecl-backed, so a
    // reference to one of these must never be treated as a user-defined generic instantiation.
    // "List"/"Stack"/"Deque"/"Queue"/"PriorityQueue"/"LinkedList"/"Map"/"Set"/"SortedMap"/
    // "SortedSet" are deliberately NOT here (see docs/language/0006-generics.md's own port
    // follow-up, docs/language/0034-maps-and-sets.md's own "2026 Update", and
    // docs/language/0040-sorted-maps.md's own "2026 Update" and docs/language/0041-sorted-sets.md's
    // own "2026 Update") - all ten are real, user-declared generic structs now
    // (std/collections.ax).
    bool isBuiltinGenericName(const std::string& name)
    {
        static const std::unordered_set<std::string> builtins{
            "slice",
            "Optional",   "Shared",
            "Result",
        };
        return builtins.contains(name);
    }

    // "Name<Arg1,Arg2,...>" -> ("Name", ["Arg1", "Arg2", ...]), depth-aware so a nested argument
    // like "Box<List<i32>>" splits correctly (mirrors the same nested-bracket handling
    // Parser::parseTypeNameAtom's own generic fallback already relies on).
    std::pair<std::string, std::vector<std::string>> splitGenericInstantiation(
        const std::string& text)
    {
        const auto lt = text.find('<');
        std::string name = text.substr(0, lt);
        std::vector<std::string> args;
        std::string current;
        int depth = 0;
        for (std::size_t i = lt + 1; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == '<')
            {
                ++depth;
                current += c;
            }
            else if (c == '>')
            {
                if (depth == 0)
                {
                    args.push_back(current);
                    current.clear();
                    break;
                }
                --depth;
                current += c;
            }
            else if (c == ',' && depth == 0)
            {
                args.push_back(current);
                current.clear();
            }
            else
            {
                current += c;
            }
        }
        return {name, args};
    }

    std::string mangleTypeText(const std::string& text)
    {
        if (text.find('<') == std::string::npos)
        {
            return text;
        }
        const auto [name, args] = splitGenericInstantiation(text);
        std::string result = name;
        for (const auto& arg : args)
        {
            result += "$" + mangleTypeText(arg);
        }
        return result;
    }

    // Whole-token substitution of a generic struct's own type parameters (e.g. "T") into a field
    // type - never a substring match, so a type parameter named "T" never corrupts an unrelated
    // identifier merely containing "T". Handles both the bare case ("T" -> "i32", the only shape
    // any Milestone-1 example actually needs) and a type parameter nested inside a larger type
    // expression ("List<T>" -> "List<i32>"), token-scanning on the same identifier-character rule
    // the lexer itself uses.
    std::string substituteTypeParams(const std::string& typeText,
                                     const std::unordered_map<std::string, std::string>& subst)
    {
        if (const auto it = subst.find(typeText); it != subst.end())
        {
            return it->second;
        }
        std::string result;
        std::string token;
        auto flush = [&]()
        {
            if (!token.empty())
            {
                const auto it = subst.find(token);
                result += it != subst.end() ? it->second : token;
                token.clear();
            }
        };
        for (const char c : typeText)
        {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            {
                token += c;
            }
            else
            {
                flush();
                result += c;
            }
        }
        flush();
        return result;
    }

    void collectTypeRefsInExpr(Expr& expr, std::vector<std::string*>& refs,
                            std::vector<CallExpr*>& callSites,
                            std::vector<MethodCallExpr*>& moduleGenericCalls);

    void collectTypeRefsInBlock(BlockExpr& block, std::vector<std::string*>& refs,
                                std::vector<CallExpr*>& callSites,
                                std::vector<MethodCallExpr*>& moduleGenericCalls)
    {
        for (auto& stmt : block.statements)
        {
            if (auto* s = dynamic_cast<AssignmentStmt*>(stmt.get()))
            {
                if (s->declaredType)
                {
                    refs.push_back(&*s->declaredType);
                }
                collectTypeRefsInExpr(*s->value, refs, callSites, moduleGenericCalls);
            }
            else if (auto* s = dynamic_cast<ReturnStmt*>(stmt.get()))
            {
                if (s->value)
                {
                    collectTypeRefsInExpr(*s->value, refs, callSites, moduleGenericCalls);
                }
            }
            else if (auto* s = dynamic_cast<WhileStmt*>(stmt.get()))
            {
                collectTypeRefsInExpr(*s->condition, refs, callSites, moduleGenericCalls);
                collectTypeRefsInExpr(*s->body, refs, callSites, moduleGenericCalls);
            }
            else if (auto* s = dynamic_cast<BreakStmt*>(stmt.get()))
            {
                if (s->value)
                {
                    collectTypeRefsInExpr(*s->value, refs, callSites, moduleGenericCalls);
                }
            }
            else if (auto* s = dynamic_cast<ExprStmt*>(stmt.get()))
            {
                collectTypeRefsInExpr(*s->expr, refs, callSites, moduleGenericCalls);
            }
            else if (auto* s = dynamic_cast<FieldAssignStmt*>(stmt.get()))
            {
                collectTypeRefsInExpr(*s->object, refs, callSites, moduleGenericCalls);
                collectTypeRefsInExpr(*s->value, refs, callSites, moduleGenericCalls);
            }
            else if (auto* s = dynamic_cast<IndexAssignStmt*>(stmt.get()))
            {
                collectTypeRefsInExpr(*s->object, refs, callSites, moduleGenericCalls);
                collectTypeRefsInExpr(*s->index, refs, callSites, moduleGenericCalls);
                collectTypeRefsInExpr(*s->value, refs, callSites, moduleGenericCalls);
            }
            else if (auto* s = dynamic_cast<IncDecStmt*>(stmt.get()))
            {
                collectTypeRefsInExpr(*s->target, refs, callSites, moduleGenericCalls);
            }
            // `*ptr = value` (see docs/language/0019-unsafe.md) - a real, previously-
            // undiscovered gap found while porting LinkedList<T>'s own self-referential
            // `*Node<T>` nodes: `*newNode = Node<T> { ... }` needs its own generic struct
            // literal walked/rewritten the identical way FieldAssignStmt's own value already is
            // just above, or the literal's un-mangled "Node<T>" typeName reaches TypeChecker
            // verbatim ("unsupported type: Node<i32>") - never exercised before this port, since
            // no earlier generic struct's own method body ever wrote through a raw pointer to a
            // *generic* struct literal.
            else if (auto* s = dynamic_cast<DerefAssignStmt*>(stmt.get()))
            {
                collectTypeRefsInExpr(*s->pointer, refs, callSites, moduleGenericCalls);
                collectTypeRefsInExpr(*s->value, refs, callSites, moduleGenericCalls);
            }
            // ContinueStmt: nothing to collect.
        }
        if (block.result)
        {
            collectTypeRefsInExpr(*block.result, refs, callSites, moduleGenericCalls);
        }
    }

    void collectTypeRefsInExpr(Expr& expr, std::vector<std::string*>& refs,
                               std::vector<CallExpr*>& callSites,
                               std::vector<MethodCallExpr*>& moduleGenericCalls)
    {
        if (auto* e = dynamic_cast<BinaryExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->left, refs, callSites, moduleGenericCalls);
            collectTypeRefsInExpr(*e->right, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<CastExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->operand, refs, callSites, moduleGenericCalls);
            refs.push_back(&e->targetType);
        }
        else if (auto* e = dynamic_cast<SizeOfExpr*>(&expr))
        {
            refs.push_back(&e->typeName);
        }
        else if (auto* e = dynamic_cast<HashOfExpr*>(&expr))
        {
            refs.push_back(&e->typeName);
            collectTypeRefsInExpr(*e->value, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<KeyEqExpr*>(&expr))
        {
            refs.push_back(&e->typeName);
            collectTypeRefsInExpr(*e->left, refs, callSites, moduleGenericCalls);
            collectTypeRefsInExpr(*e->right, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<SomeExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->value, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<ShareExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->value, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<OkExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->value, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<ErrExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->value, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<TryExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->operand, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<InterpolatedStringExpr*>(&expr))
        {
            for (auto& piece : e->pieces)
            {
                if (piece.expr)
                {
                    collectTypeRefsInExpr(*piece.expr, refs, callSites, moduleGenericCalls);
                }
            }
        }
        else if (auto* e = dynamic_cast<IfExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->condition, refs, callSites, moduleGenericCalls);
            collectTypeRefsInExpr(*e->thenBranch, refs, callSites, moduleGenericCalls);
            if (e->elseBranch)
            {
                collectTypeRefsInExpr(*e->elseBranch, refs, callSites, moduleGenericCalls);
            }
        }
        else if (auto* e = dynamic_cast<MatchExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->scrutinee, refs, callSites, moduleGenericCalls);
            for (auto& arm : e->arms)
            {
                if (arm.body)
                {
                    collectTypeRefsInExpr(*arm.body, refs, callSites, moduleGenericCalls);
                }
            }
        }
        else if (auto* e = dynamic_cast<LoopExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->body, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<CallExpr*>(&expr))
        {
            for (auto& arg : e->arguments)
            {
                collectTypeRefsInExpr(*arg, refs, callSites, moduleGenericCalls);
            }
            // `name<Type>(args)` (see docs/language/0006-generics.md's own List<T> port
            // follow-up) - an explicit call-site type argument to a generic top-level function.
            // Collected separately from `refs` (a plain string* to rewrite in place isn't enough
            // here - both `callee` and `typeArgument` need updating together, and a synthesis
            // step needs to run, not just a substitution) - see synthesizeGenericFunctionCalls.
            if (!e->typeArgument.empty())
            {
                callSites.push_back(e);
            }
        }
        else if (auto* e = dynamic_cast<FieldExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->object, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<StructLiteralExpr*>(&expr))
        {
            refs.push_back(&e->typeName);
            for (auto& [fieldName, value] : e->fields)
            {
                collectTypeRefsInExpr(*value, refs, callSites, moduleGenericCalls);
            }
        }
        else if (auto* e = dynamic_cast<ArrayLiteralExpr*>(&expr))
        {
            for (auto& element : e->elements)
            {
                collectTypeRefsInExpr(*element, refs, callSites, moduleGenericCalls);
            }
        }
        else if (auto* e = dynamic_cast<IndexExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->object, refs, callSites, moduleGenericCalls);
            collectTypeRefsInExpr(*e->index, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<StrSliceExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->object, refs, callSites, moduleGenericCalls);
            if (e->start)
            {
                collectTypeRefsInExpr(*e->start, refs, callSites, moduleGenericCalls);
            }
            if (e->end)
            {
                collectTypeRefsInExpr(*e->end, refs, callSites, moduleGenericCalls);
            }
        }
        else if (auto* e = dynamic_cast<MethodCallExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->object, refs, callSites, moduleGenericCalls);
            // `Box<i32>.new(41)` (see docs/language/0068-c-style-syntax.md) - an associated
            // function called on an *explicit* generic instantiation. The parser already
            // canonicalized `object` to an ordinary NameExpr carrying the bracket-syntax type
            // text ("Box<i32>") rather than a bare struct name (see parsePrimary's own
            // looksLikeGenericTypeRefBeforeDot branch) - collected as a type ref here exactly like
            // any other type-position reference, so the fixed-point loop below registers/
            // synthesizes the real "Box$i32" instantiation (including its own embedded methods,
            // via synthesizeGenericImplMethods) and rewrites this NameExpr's own text in place to
            // match. By the time TypeChecker et al. see it, `object` is an ordinary
            // NameExpr("Box$i32") - indistinguishable from any other associated-function call
            // (`Counter.new(5)`'s own NameExpr("Counter") never has a '<', so this never fires for
            // that case).
            if (auto* objectName = dynamic_cast<NameExpr*>(e->object.get());
                objectName && objectName->name.find('<') != std::string::npos)
            {
                refs.push_back(&objectName->name);
            }
            for (auto& arg : e->arguments)
            {
                collectTypeRefsInExpr(*arg, refs, callSites, moduleGenericCalls);
            }
            if (!e->typeArgument.empty())
            {
                refs.push_back(&e->typeArgument);
                // `module.name<Type>(args)` (see docs/language/0006-generics.md's own List<T>
                // port follow-up) - an explicit call-site type argument to a generic function
                // reached via module-qualified syntax (e.g. `collections.newList<i32>()`),
                // parsed as an ordinary MethodCallExpr (see docs/language/0066-modules.md)
                // rather than a CallExpr. Collected whenever `object` is a bare name -
                // synthesizeGenericFunctionCall's own caller decides whether "object.method"
                // actually names a known generic function template; an ordinary generic struct
                // method call with an explicit type argument (none exist yet in this codebase)
                // would just fail that lookup and fall through untouched.
                if (dynamic_cast<const NameExpr*>(e->object.get()))
                {
                    moduleGenericCalls.push_back(e);
                }
            }
        }
        else if (auto* e = dynamic_cast<StringNewExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->text, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<BlockExpr*>(&expr))
        {
            collectTypeRefsInBlock(*e, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<ClosureExpr*>(&expr))
        {
            for (auto& param : e->params)
            {
                refs.push_back(&param.type);
            }
            if (e->returnType)
            {
                refs.push_back(&*e->returnType);
            }
            collectTypeRefsInExpr(*e->body, refs, callSites, moduleGenericCalls);
        }
        // `unsafe { ... }`/`*ptr`/`&name` (see docs/language/0019-unsafe.md) - three real,
        // previously-undiscovered gaps found while porting LinkedList<T>'s own self-referential
        // `*Node<T>` nodes: none of these three were ever walked here at all (cloneExpr, the
        // sibling function that deep-clones a monomorphized method body, already has all three -
        // this function's own "IntegerExpr/.../BufferNewExpr: leaves" comment just below was
        // simply wrong about UnsafeBlockExpr/DerefExpr/AddressOfExpr, which do carry nested
        // expressions). Harmless before now only because no earlier collection's own `unsafe`
        // block ever happened to contain a *generic* struct literal or reference needing
        // mangling - `unsafe { *newNode = Node<T> { ... } }`'s own inner struct literal was
        // silently never visited, reaching TypeChecker with its un-mangled "Node<T>" typeName
        // still intact ("unsupported type: Node<i32>").
        else if (auto* e = dynamic_cast<UnsafeBlockExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->body, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<DerefExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->operand, refs, callSites, moduleGenericCalls);
        }
        else if (auto* e = dynamic_cast<AddressOfExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->operand, refs, callSites, moduleGenericCalls);
        }
        // IntegerExpr/Int64Expr/FloatExpr/NameExpr/NoneExpr/NullExpr/BoolExpr/StringExpr/
        // CharExpr/BufferNewExpr: leaves with no nested expression or type-string to collect.
    }

    // Deep-clones a method body (or any nested expression/statement inside one), applying
    // `subst`'s whole-token type-parameter substitution to every embedded type-text field along
    // the way (self's own param type, every other param/return type, and any nested type text -
    // a struct literal's typeName, a collection constructor's elementType, a cast's targetType,
    // ...) - the exact same set of fields collectTypeRefsInExpr/collectTypeRefsInBlock already
    // enumerate for the ordinary (non-cloning) rewrite pass below, mirrored here field-for-field.
    // Substitution happens immediately during the clone, not deferred to that later pass, because
    // a *bare* type-parameter reference (e.g. return type "T" alone, with no surrounding "<...>")
    // has no '<' for that pass's own `ref->find('<')` check to ever notice - only a bracket-
    // wrapped reference like "Box<T>" (produced here as "Box<i32>", not yet the final mangled
    // "Box$i32") is left for that pass's own fixed-point loop to finish resolving on its next
    // iteration, exactly like an ordinary generic function body's own such reference already is.
    std::unique_ptr<Expr> cloneExpr(const Expr& expr,
                                    const std::unordered_map<std::string, std::string>& subst);
    std::unique_ptr<Stmt> cloneStmt(const Stmt& stmt,
                                    const std::unordered_map<std::string, std::string>& subst);

    std::unique_ptr<BlockExpr> cloneBlock(const BlockExpr& block,
                                          const std::unordered_map<std::string, std::string>& subst)
    {
        std::vector<std::unique_ptr<Stmt>> statements;
        statements.reserve(block.statements.size());
        for (const auto& stmt : block.statements)
        {
            statements.push_back(cloneStmt(*stmt, subst));
        }
        auto result = block.result ? cloneExpr(*block.result, subst) : nullptr;
        return std::make_unique<BlockExpr>(std::move(statements), std::move(result));
    }

    std::unique_ptr<Stmt> cloneStmt(const Stmt& stmt,
                                    const std::unordered_map<std::string, std::string>& subst)
    {
        if (const auto* s = dynamic_cast<const AssignmentStmt*>(&stmt))
        {
            std::optional<std::string> declaredType;
            if (s->declaredType)
            {
                declaredType = substituteTypeParams(*s->declaredType, subst);
            }
            return std::make_unique<AssignmentStmt>(
                s->name, std::move(declaredType), cloneExpr(*s->value, subst), s->forceDefine);
        }
        if (const auto* s = dynamic_cast<const ReturnStmt*>(&stmt))
        {
            return std::make_unique<ReturnStmt>(s->value ? cloneExpr(*s->value, subst) : nullptr);
        }
        if (const auto* s = dynamic_cast<const WhileStmt*>(&stmt))
        {
            return std::make_unique<WhileStmt>(cloneExpr(*s->condition, subst),
                                               cloneExpr(*s->body, subst));
        }
        if (const auto* s = dynamic_cast<const BreakStmt*>(&stmt))
        {
            return std::make_unique<BreakStmt>(s->value ? cloneExpr(*s->value, subst) : nullptr);
        }
        if (dynamic_cast<const ContinueStmt*>(&stmt))
        {
            return std::make_unique<ContinueStmt>();
        }
        if (const auto* s = dynamic_cast<const ExprStmt*>(&stmt))
        {
            return std::make_unique<ExprStmt>(cloneExpr(*s->expr, subst));
        }
        if (const auto* s = dynamic_cast<const FieldAssignStmt*>(&stmt))
        {
            return std::make_unique<FieldAssignStmt>(
                cloneExpr(*s->object, subst), s->field, cloneExpr(*s->value, subst));
        }
        if (const auto* s = dynamic_cast<const IndexAssignStmt*>(&stmt))
        {
            return std::make_unique<IndexAssignStmt>(cloneExpr(*s->object, subst),
                                                      cloneExpr(*s->index, subst),
                                                      cloneExpr(*s->value, subst));
        }
        if (const auto* s = dynamic_cast<const DerefAssignStmt*>(&stmt))
        {
            return std::make_unique<DerefAssignStmt>(cloneExpr(*s->pointer, subst),
                                                      cloneExpr(*s->value, subst));
        }
        if (const auto* s = dynamic_cast<const IncDecStmt*>(&stmt))
        {
            return std::make_unique<IncDecStmt>(cloneExpr(*s->target, subst), s->increment);
        }
        throw std::runtime_error(
            "GenericMonomorphizer: unsupported statement kind inside a generic impl method body");
    }

    std::unique_ptr<Expr> cloneExpr(const Expr& expr,
                                    const std::unordered_map<std::string, std::string>& subst)
    {
        if (const auto* e = dynamic_cast<const IntegerExpr*>(&expr))
        {
            return std::make_unique<IntegerExpr>(e->value);
        }
        if (const auto* e = dynamic_cast<const Int64Expr*>(&expr))
        {
            return std::make_unique<Int64Expr>(e->value);
        }
        if (const auto* e = dynamic_cast<const FloatExpr*>(&expr))
        {
            return std::make_unique<FloatExpr>(e->value);
        }
        if (const auto* e = dynamic_cast<const NameExpr*>(&expr))
        {
            return std::make_unique<NameExpr>(e->name);
        }
        if (const auto* e = dynamic_cast<const BinaryExpr*>(&expr))
        {
            return std::make_unique<BinaryExpr>(
                cloneExpr(*e->left, subst), e->op, cloneExpr(*e->right, subst));
        }
        if (const auto* e = dynamic_cast<const CastExpr*>(&expr))
        {
            return std::make_unique<CastExpr>(cloneExpr(*e->operand, subst),
                                              substituteTypeParams(e->targetType, subst));
        }
        if (const auto* e = dynamic_cast<const SizeOfExpr*>(&expr))
        {
            return std::make_unique<SizeOfExpr>(substituteTypeParams(e->typeName, subst));
        }
        if (const auto* e = dynamic_cast<const HashOfExpr*>(&expr))
        {
            return std::make_unique<HashOfExpr>(substituteTypeParams(e->typeName, subst),
                                                cloneExpr(*e->value, subst));
        }
        if (const auto* e = dynamic_cast<const KeyEqExpr*>(&expr))
        {
            return std::make_unique<KeyEqExpr>(substituteTypeParams(e->typeName, subst),
                                               cloneExpr(*e->left, subst),
                                               cloneExpr(*e->right, subst));
        }
        if (const auto* e = dynamic_cast<const DerefExpr*>(&expr))
        {
            return std::make_unique<DerefExpr>(cloneExpr(*e->operand, subst));
        }
        if (const auto* e = dynamic_cast<const AddressOfExpr*>(&expr))
        {
            return std::make_unique<AddressOfExpr>(cloneExpr(*e->operand, subst));
        }
        if (const auto* e = dynamic_cast<const UnsafeBlockExpr*>(&expr))
        {
            return std::make_unique<UnsafeBlockExpr>(cloneExpr(*e->body, subst));
        }
        if (const auto* e = dynamic_cast<const SomeExpr*>(&expr))
        {
            return std::make_unique<SomeExpr>(cloneExpr(*e->value, subst));
        }
        if (dynamic_cast<const NoneExpr*>(&expr))
        {
            return std::make_unique<NoneExpr>();
        }
        if (dynamic_cast<const NullExpr*>(&expr))
        {
            return std::make_unique<NullExpr>();
        }
        if (const auto* e = dynamic_cast<const ShareExpr*>(&expr))
        {
            return std::make_unique<ShareExpr>(cloneExpr(*e->value, subst));
        }
        if (const auto* e = dynamic_cast<const OkExpr*>(&expr))
        {
            return std::make_unique<OkExpr>(cloneExpr(*e->value, subst));
        }
        if (const auto* e = dynamic_cast<const ErrExpr*>(&expr))
        {
            return std::make_unique<ErrExpr>(cloneExpr(*e->value, subst));
        }
        if (const auto* e = dynamic_cast<const TryExpr*>(&expr))
        {
            return std::make_unique<TryExpr>(cloneExpr(*e->operand, subst));
        }
        if (const auto* e = dynamic_cast<const BoolExpr*>(&expr))
        {
            return std::make_unique<BoolExpr>(e->value);
        }
        if (const auto* e = dynamic_cast<const StringExpr*>(&expr))
        {
            return std::make_unique<StringExpr>(e->value);
        }
        if (const auto* e = dynamic_cast<const InterpolatedStringExpr*>(&expr))
        {
            std::vector<InterpolatedStringExpr::Piece> pieces;
            pieces.reserve(e->pieces.size());
            for (const auto& piece : e->pieces)
            {
                InterpolatedStringExpr::Piece cloned;
                cloned.literalText = piece.literalText;
                cloned.expr = piece.expr ? cloneExpr(*piece.expr, subst) : nullptr;
                cloned.formatSpec = piece.formatSpec;
                cloned.selfDocPrefix = piece.selfDocPrefix;
                cloned.debug = piece.debug;
                pieces.push_back(std::move(cloned));
            }
            return std::make_unique<InterpolatedStringExpr>(std::move(pieces));
        }
        if (const auto* e = dynamic_cast<const CharExpr*>(&expr))
        {
            return std::make_unique<CharExpr>(e->codepoint);
        }
        if (const auto* e = dynamic_cast<const IfExpr*>(&expr))
        {
            return std::make_unique<IfExpr>(cloneExpr(*e->condition, subst),
                                            cloneExpr(*e->thenBranch, subst),
                                            e->elseBranch ? cloneExpr(*e->elseBranch, subst)
                                                          : nullptr);
        }
        if (const auto* e = dynamic_cast<const MatchExpr*>(&expr))
        {
            std::vector<MatchArm> arms;
            arms.reserve(e->arms.size());
            for (const auto& arm : e->arms)
            {
                MatchArm cloned;
                cloned.variantName = arm.variantName;
                cloned.bindingNames = arm.bindingNames;
                cloned.body = arm.body ? cloneExpr(*arm.body, subst) : nullptr;
                arms.push_back(std::move(cloned));
            }
            return std::make_unique<MatchExpr>(cloneExpr(*e->scrutinee, subst), std::move(arms));
        }
        if (const auto* e = dynamic_cast<const LoopExpr*>(&expr))
        {
            return std::make_unique<LoopExpr>(cloneExpr(*e->body, subst));
        }
        if (const auto* e = dynamic_cast<const CallExpr*>(&expr))
        {
            std::vector<std::unique_ptr<Expr>> args;
            args.reserve(e->arguments.size());
            for (const auto& arg : e->arguments)
            {
                args.push_back(cloneExpr(*arg, subst));
            }
            // `name<Type>(args)` (see docs/language/0006-generics.md's own List<T> port
            // follow-up) - a nested generic call inside a generic function/method body (e.g.
            // `newStack<T>`'s own `newList<T>()`) must keep its own type argument, substituted
            // the same way any other type-text field is, so the outer clone's own fixed-point
            // rewrite can resolve it on a later iteration - dropping it here (as an earlier,
            // now-fixed version of this function did) left the inner call bare ("newList()"),
            // permanently unresolvable.
            std::string typeArgument =
                e->typeArgument.empty() ? std::string() : substituteTypeParams(e->typeArgument, subst);
            return std::make_unique<CallExpr>(e->callee, std::move(args), std::move(typeArgument));
        }
        if (const auto* e = dynamic_cast<const FieldExpr*>(&expr))
        {
            return std::make_unique<FieldExpr>(cloneExpr(*e->object, subst), e->field);
        }
        if (const auto* e = dynamic_cast<const StructLiteralExpr*>(&expr))
        {
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> fields;
            fields.reserve(e->fields.size());
            for (const auto& [name, value] : e->fields)
            {
                fields.emplace_back(name, cloneExpr(*value, subst));
            }
            return std::make_unique<StructLiteralExpr>(substituteTypeParams(e->typeName, subst),
                                                        std::move(fields));
        }
        if (const auto* e = dynamic_cast<const ArrayLiteralExpr*>(&expr))
        {
            std::vector<std::unique_ptr<Expr>> elements;
            elements.reserve(e->elements.size());
            for (const auto& element : e->elements)
            {
                elements.push_back(cloneExpr(*element, subst));
            }
            return std::make_unique<ArrayLiteralExpr>(std::move(elements));
        }
        if (const auto* e = dynamic_cast<const IndexExpr*>(&expr))
        {
            return std::make_unique<IndexExpr>(cloneExpr(*e->object, subst),
                                               cloneExpr(*e->index, subst));
        }
        if (const auto* e = dynamic_cast<const StrSliceExpr*>(&expr))
        {
            return std::make_unique<StrSliceExpr>(cloneExpr(*e->object, subst),
                                                  e->start ? cloneExpr(*e->start, subst) : nullptr,
                                                  e->end ? cloneExpr(*e->end, subst) : nullptr);
        }
        if (const auto* e = dynamic_cast<const MethodCallExpr*>(&expr))
        {
            std::vector<std::unique_ptr<Expr>> args;
            args.reserve(e->arguments.size());
            for (const auto& arg : e->arguments)
            {
                args.push_back(cloneExpr(*arg, subst));
            }
            std::string typeArgument =
                e->typeArgument.empty() ? std::string() : substituteTypeParams(e->typeArgument, subst);
            // `SomeType<T>.assocFn(...)` (see docs/language/0068-c-style-syntax.md) called from
            // *inside* another generic function/method's own body, using that enclosing generic's
            // own type param T - e->object is a NameExpr carrying bracket-syntax type text
            // ("SomeType<T>"), which plain cloneExpr(*e->object, subst) would copy verbatim (its
            // own NameExpr case never substitutes type params within a name - ordinary variable
            // names never contain any). Substituted the same way typeArgument just above already
            // is, so "SomeType<T>" correctly becomes e.g. "SomeType<i32>" per instantiation,
            // matching what collectTypeRefsInExpr's own identical NameExpr-with-'<' check expects
            // to find.
            auto* objectName = dynamic_cast<const NameExpr*>(e->object.get());
            auto object = objectName && objectName->name.find('<') != std::string::npos
                              ? std::make_unique<NameExpr>(substituteTypeParams(objectName->name, subst))
                              : cloneExpr(*e->object, subst);
            return std::make_unique<MethodCallExpr>(
                std::move(object), e->method, std::move(args), std::move(typeArgument));
        }
        if (const auto* e = dynamic_cast<const StringNewExpr*>(&expr))
        {
            return std::make_unique<StringNewExpr>(cloneExpr(*e->text, subst));
        }
        if (dynamic_cast<const BufferNewExpr*>(&expr))
        {
            return std::make_unique<BufferNewExpr>();
        }
        if (const auto* e = dynamic_cast<const BlockExpr*>(&expr))
        {
            return cloneBlock(*e, subst);
        }
        if (const auto* e = dynamic_cast<const ClosureExpr*>(&expr))
        {
            std::vector<Param> params;
            params.reserve(e->params.size());
            for (const auto& param : e->params)
            {
                params.push_back(
                    Param{param.name, substituteTypeParams(param.type, subst), param.declaredCapability});
            }
            std::optional<std::string> returnType;
            if (e->returnType)
            {
                returnType = substituteTypeParams(*e->returnType, subst);
            }
            return std::make_unique<ClosureExpr>(
                std::move(params), std::move(returnType), cloneExpr(*e->body, subst));
        }
        throw std::runtime_error(
            "GenericMonomorphizer: unsupported expression kind inside a generic impl method body");
    }

    // Every live std::string* that might hold a generic-instantiation reference, anywhere in the
    // program. StructDecl fields are collected only for a *concrete* struct (typeParams empty) -
    // a generic template's own fields legitimately contain an unsubstituted type-parameter
    // placeholder (e.g. "T", or "List<T>"), which must never be looked up as if it were a real
    // instantiation; only after GenericMonomorphizer substitutes a template's fields into a fresh
    // concrete synthesized StructDecl (added to program.items with an empty typeParams) do those
    // substituted field types become eligible for the next fixed-point iteration.
    void collectGenericReferences(Program& program,
                                  std::vector<std::string*>& refs,
                                  std::vector<CallExpr*>& callSites,
                                  std::vector<MethodCallExpr*>& moduleGenericCalls)
    {
        for (auto& item : program.items)
        {
            if (auto* s = dynamic_cast<StructDecl*>(item.get()))
            {
                if (s->typeParams.empty())
                {
                    for (auto& field : s->fields)
                    {
                        refs.push_back(&field.type);
                    }
                }
            }
            else if (auto* f = dynamic_cast<FunctionDecl*>(item.get()))
            {
                // A generic top-level function template's own body is never scanned here
                // (mirrors StructDecl/ImplDecl's identical typeParams-non-empty exclusion) - its
                // unsubstituted parameter/return types aren't real instantiation references.
                // Only GenericMonomorphizer's own synthesized, concrete-per-call-site clones
                // (plain FunctionDecl items with an empty typeParams) are - picked up by this
                // same branch, on their own.
                if (f->typeParams.empty())
                {
                    for (auto& param : f->params)
                    {
                        refs.push_back(&param.type);
                    }
                    if (f->returnType)
                    {
                        refs.push_back(&*f->returnType);
                    }
                    collectTypeRefsInExpr(*f->body, refs, callSites, moduleGenericCalls);
                }
            }
            else if (auto* e = dynamic_cast<ExternDecl*>(item.get()))
            {
                for (auto& param : e->params)
                {
                    refs.push_back(&param.type);
                }
                if (e->returnType)
                {
                    refs.push_back(&*e->returnType);
                }
            }
            else if (auto* a = dynamic_cast<AssignmentStmt*>(item.get()))
            {
                if (a->declaredType)
                {
                    refs.push_back(&*a->declaredType);
                }
                collectTypeRefsInExpr(*a->value, refs, callSites, moduleGenericCalls);
            }
            else if (auto* es = dynamic_cast<ExprStmt*>(item.get()))
            {
                collectTypeRefsInExpr(*es->expr, refs, callSites, moduleGenericCalls);
            }
            else if (auto* impl = dynamic_cast<ImplDecl*>(item.get()))
            {
                // A generic impl template's own methods are never scanned here (mirrors
                // StructDecl's identical typeParams-non-empty exclusion above) - their
                // unsubstituted "self: Box<T>" text isn't a real instantiation reference (it
                // would spuriously match declsByName["Box"] with argStrings=["T"], synthesizing
                // a bogus "Box$T" struct with an unresolvable "T" field). Only a *concrete*
                // impl's methods are scanned, so a body like `impl Point { wrap(self) -> Box<i32>
                // {...} }` still gets its own `Box<i32>` resolved, same as any ordinary function
                // body already does. A generic impl's own *synthesized* (monomorphized) methods
                // need no separate case here either - synthesizeGenericImplMethods below appends
                // them as plain top-level FunctionDecl items, already covered by the branch above.
                if (impl->typeParams.empty())
                {
                    for (auto& method : impl->methods)
                    {
                        for (auto& param : method->params)
                        {
                            refs.push_back(&param.type);
                        }
                        if (method->returnType)
                        {
                            refs.push_back(&*method->returnType);
                        }
                        collectTypeRefsInExpr(*method->body, refs, callSites, moduleGenericCalls);
                    }
                }
            }
            // TraitDecl/EnumDecl/ModuleDecl/UseDecl: no generic-struct instantiation reference
            // can appear in any of these.
        }
    }
    // For every `impl<...> genericName<...> { methods }` block whose own type-param arity
    // matches the struct template just instantiated, clones+substitutes each method and appends
    // it as a plain top-level FunctionDecl, mangled name `mangled + "." + methodName` (e.g.
    // "Box$i32.get") - every downstream pass's own "loop over program.items, register any
    // FunctionDecl" code then picks it up for free, exactly like a concrete, hand-written impl
    // method already is. Runs once per newly-synthesized `mangled` instantiation (called only
    // from inside monomorphizeGenerics's own `!declsByName.contains(mangled)` guard).
    void synthesizeGenericImplMethods(Program& program,
                                      const std::string& genericName,
                                      const std::string& mangled,
                                      const std::unordered_map<std::string, std::string>& subst)
    {
        // Snapshot the raw pointers first - appending to program.items below would otherwise
        // invalidate this loop's own iterator into that same vector.
        std::vector<ImplDecl*> matchingImpls;
        for (auto& item : program.items)
        {
            if (auto* impl = dynamic_cast<ImplDecl*>(item.get());
                impl && impl->typeName == genericName && impl->typeParams.size() == subst.size())
            {
                matchingImpls.push_back(impl);
            }
        }

        for (const ImplDecl* impl : matchingImpls)
        {
            for (const auto& method : impl->methods)
            {
                std::vector<Param> params;
                params.reserve(method->params.size());
                for (const auto& param : method->params)
                {
                    params.push_back(Param{
                        param.name, substituteTypeParams(param.type, subst), param.declaredCapability});
                }
                std::optional<std::string> returnType;
                if (method->returnType)
                {
                    returnType = substituteTypeParams(*method->returnType, subst);
                }
                // method->name is "genericName.methodName" (see Parser::parseImplMethod) - strip
                // the generic template's own bare name, re-prefix with the mangled one.
                const std::string methodName = method->name.substr(genericName.size() + 1);
                auto clone = std::make_unique<FunctionDecl>(mangled + "." + methodName,
                                                            std::move(params),
                                                            std::move(returnType),
                                                            cloneExpr(*method->body, subst));
                // A real, previously-undiscovered bug found while adding
                // `Box<i32>.new(...)`-style associated-call support on generic instantiations (see
                // docs/language/0068-c-style-syntax.md): `isPublic` was never copied from the
                // template method onto its synthesized clone here, defaulting to false regardless
                // of the source's own `pub`. Harmless before that feature - an impl method's own
                // `pub`-ness never affected anything, since ordinary `obj.method()` dispatch never
                // checks it - but a self-less associated function reached via the
                // module-qualified-call mechanism (`moduleNames_`, see that same doc) *does* check
                // `isPublic`, so a `pub`-declared generic associated function was silently treated
                // as private once monomorphized.
                clone->isPublic = method->isPublic;
                program.items.push_back(std::move(clone));
            }
        }
    }
} // namespace

// Synthesizes a mangled clone of a generic top-level function template for one-or-more concrete
// call-site type arguments (see docs/language/0006-generics.md's own List<T> port follow-up,
// extended to more than one type parameter by docs/language/0034-maps-and-sets.md's own "2026
// Update", e.g. `newMap<K,V>()`) - deep-clones params/returnType/body via the exact same
// `cloneExpr`/`substituteTypeParams` infra generic impl methods already use, appends the clone as
// an ordinary top-level FunctionDecl (empty typeParams of its own, picked up by every downstream
// pass's existing FunctionDecl handling for free), and rewrites the call site's own
// `callee`/`typeArgument` in place. Returns the mangled name.
std::string synthesizeGenericFunctionCall(Program& program,
                                          const FunctionDecl& tmpl,
                                          const std::string& typeArgument)
{
    // `typeArgument` is Parser::parsePrimary's own comma-joined text ("i32" or "i32,str") - split
    // the same bracket-depth-aware way splitGenericInstantiation splits a struct instantiation's
    // own argument list, by treating it as that function's own argument-list text (wrapping it in
    // a throwaway "<...>" so the existing depth-aware scanner can be reused verbatim).
    const auto argStrings = splitGenericInstantiation("<" + typeArgument + ">").second;
    if (argStrings.size() != tmpl.typeParams.size())
    {
        throw std::runtime_error("'" + tmpl.name + "' declares " +
                                 std::to_string(tmpl.typeParams.size()) +
                                 " type parameter(s), but the call site supplied " +
                                 std::to_string(argStrings.size()));
    }
    std::string mangled = tmpl.name;
    for (const auto& arg : argStrings)
    {
        mangled += "$" + mangleTypeText(arg);
    }

    const bool alreadySynthesized =
        std::any_of(program.items.begin(),
                   program.items.end(),
                   [&](const auto& item)
                   {
                       const auto* f = dynamic_cast<const FunctionDecl*>(item.get());
                       return f && f->name == mangled;
                   });
    if (!alreadySynthesized)
    {
        std::unordered_map<std::string, std::string> subst;
        for (std::size_t i = 0; i < tmpl.typeParams.size(); ++i)
        {
            subst[tmpl.typeParams[i]] = argStrings[i];
        }
        std::vector<Param> params;
        params.reserve(tmpl.params.size());
        for (const auto& param : tmpl.params)
        {
            params.push_back(Param{
                param.name, substituteTypeParams(param.type, subst), param.declaredCapability});
        }
        std::optional<std::string> returnType;
        if (tmpl.returnType)
        {
            returnType = substituteTypeParams(*tmpl.returnType, subst);
        }
        auto clone = std::make_unique<FunctionDecl>(
            mangled, std::move(params), std::move(returnType), cloneExpr(*tmpl.body, subst));
        clone->isPublic = tmpl.isPublic;
        program.items.push_back(std::move(clone));
    }
    return mangled;
}

void monomorphizeGenerics(Program& program)
{
    std::unordered_map<std::string, StructDecl*> declsByName;
    std::unordered_map<std::string, const FunctionDecl*> functionTemplatesByName;
    for (auto& item : program.items)
    {
        if (auto* s = dynamic_cast<StructDecl*>(item.get()))
        {
            declsByName[s->name] = s;
        }
        else if (auto* f = dynamic_cast<FunctionDecl*>(item.get()); f && !f->typeParams.empty())
        {
            functionTemplatesByName[f->name] = f;
        }
    }

    bool changed = true;
    while (changed)
    {
        changed = false;
        std::vector<std::string*> refs;
        std::vector<CallExpr*> callSites;
        std::vector<MethodCallExpr*> moduleGenericCalls;
        collectGenericReferences(program, refs, callSites, moduleGenericCalls);

        for (CallExpr* call : callSites)
        {
            const auto it = functionTemplatesByName.find(call->callee);
            if (it == functionTemplatesByName.end())
            {
                throw std::runtime_error("'" + call->callee +
                                         "' is not a known generic function");
            }
            call->callee = synthesizeGenericFunctionCall(program, *it->second, call->typeArgument);
            call->typeArgument.clear();
            changed = true;
        }

        // `module.name<Type>(args)` (see docs/language/0006-generics.md's own List<T> port
        // follow-up) - a generic function reached via module-qualified syntax (e.g.
        // `collections.newList<i32>()`, see docs/language/0066-modules.md), parsed as an
        // ordinary MethodCallExpr rather than a CallExpr. main.cpp's own module-merge pass has
        // already qualified every real generic function template's own name this way
        // ("collections.newList"), so the lookup key is built the identical way. A miss here is
        // NOT an error - unlike callSites above, an arbitrary MethodCallExpr with a type
        // argument could legitimately be something else (e.g. `.parse<T>()` on an ordinary
        // local), left untouched for TypeChecker's own existing handling.
        for (MethodCallExpr* call : moduleGenericCalls)
        {
            const auto* objectName = static_cast<const NameExpr*>(call->object.get());
            const std::string qualifiedName = objectName->name + "." + call->method;
            const auto it = functionTemplatesByName.find(qualifiedName);
            if (it == functionTemplatesByName.end())
            {
                continue;
            }
            const std::string mangled =
                synthesizeGenericFunctionCall(program, *it->second, call->typeArgument);
            call->method = mangled.substr(objectName->name.size() + 1);
            call->typeArgument.clear();
            changed = true;
        }

        for (std::string* ref : refs)
        {
            if (ref->find('<') == std::string::npos)
            {
                continue;
            }

            // `*Node<T>`/`**MapEntry<K,V>` (see docs/language/0019-unsafe.md,
            // docs/language/0036-linked-lists.md's own 2026 self-referential-node port, and
            // docs/language/0034-maps-and-sets.md's own 2026 update - a bucket array's own
            // `**Entry<K,V>` field needed a *second* leading star, previously-undiscovered since
            // LinkedList<T>'s own `*Node<T>` only ever needed one). Two real, previously-
            // undiscovered bugs found while porting LinkedList<T>, both fixed here together (and
            // generalized here from "at most one star" to "any number of stars" for Map<K,V>'s own
            // sake):
            // 1. splitGenericInstantiation on the full "*Node<T>" text (leading star(s) included)
            //    returned genericName "*Node", which declsByName never contains (it's keyed by
            //    the bare struct name) - "unknown generic struct '*Node'". Fixed by stripping every
            //    leading star for this lookup only (`lookupText`).
            // 2. Worse, and only surfacing once (1) was fixed: `mangled` (used both to *name* a
            //    newly-synthesized struct instantiation and as `declsByName`'s own key for it)
            //    was computed from the *un-stripped* `*ref` text - a struct's own identity has no
            //    star (there is exactly one "Node$i32", referenced from many places, some by
            //    value and some by pointer), but this synthesized a second, bogus struct literally
            //    *named* "*Node$i32", whose own fields (cloned from the same template) included
            //    another `*Node$i32`-typed field pointing at itself by that same bogus name -
            //    genuine infinite self-reference by construction, not merely by algorithm,
            //    crashing axeaTypeByteSize's own recursive struct-size computation with a real
            //    stack overflow the first time anything computed `sizeof<Node<T>>()`. Fixed by
            //    computing `mangled` from `lookupText` (star-free) too, so the struct is always
            //    synthesized/registered under its one true bare name - the same number of leading
            //    stars is re-applied only to `*ref` itself (the specific reference site being
            //    rewritten), never baked into the struct's own identity.
            std::size_t starCount = 0;
            while (starCount < ref->size() && (*ref)[starCount] == '*')
            {
                ++starCount;
            }
            const std::string stars(starCount, '*');
            const std::string lookupText = ref->substr(starCount);
            const auto [genericName, argStrings] = splitGenericInstantiation(lookupText);
            if (isBuiltinGenericName(genericName))
            {
                continue;
            }

            const auto it = declsByName.find(genericName);
            if (it == declsByName.end())
            {
                throw std::runtime_error("unknown generic struct '" + genericName + "'");
            }
            const StructDecl& tmpl = *it->second;
            if (tmpl.typeParams.empty())
            {
                throw std::runtime_error("'" + genericName + "' is not a generic struct");
            }
            if (argStrings.size() != tmpl.typeParams.size())
            {
                throw std::runtime_error(
                    "'" + genericName + "' expects " + std::to_string(tmpl.typeParams.size()) +
                    " type argument(s), got " + std::to_string(argStrings.size()));
            }

            const std::string mangled = mangleTypeText(lookupText);
            if (!declsByName.contains(mangled))
            {
                std::unordered_map<std::string, std::string> subst;
                for (std::size_t i = 0; i < tmpl.typeParams.size(); ++i)
                {
                    subst[tmpl.typeParams[i]] = argStrings[i];
                }
                std::vector<Field> fields;
                for (const auto& field : tmpl.fields)
                {
                    fields.push_back(Field{field.name, substituteTypeParams(field.type, subst)});
                }

                auto synthesized = std::make_unique<StructDecl>(mangled, std::move(fields));
                declsByName[mangled] = synthesized.get();
                program.items.push_back(std::move(synthesized));
                synthesizeGenericImplMethods(program, genericName, mangled, subst);
                changed = true;
            }

            *ref = stars + mangled;
            changed = true;
        }
    }
}
