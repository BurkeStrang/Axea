#include "generics/GenericMonomorphizer.hpp"

#include "ast/Expr.hpp"

#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // Every built-in generic-shaped type name already hardcoded in
    // Parser::parseTypeNameAtom/TypeChecker::resolveType - never StructDecl-backed, so a
    // reference to one of these must never be treated as a user-defined generic instantiation.
    bool isBuiltinGenericName(const std::string& name)
    {
        static const std::unordered_set<std::string> builtins{
            "slice", "List", "Set",  "Stack",      "LinkedList", "Deque",
            "Queue", "PriorityQueue", "SortedSet", "Optional",   "Shared",
            "Map",   "Result",        "SortedMap",
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

    void collectTypeRefsInExpr(Expr& expr, std::vector<std::string*>& refs);

    void collectTypeRefsInBlock(BlockExpr& block, std::vector<std::string*>& refs)
    {
        for (auto& stmt : block.statements)
        {
            if (auto* s = dynamic_cast<AssignmentStmt*>(stmt.get()))
            {
                if (s->declaredType)
                {
                    refs.push_back(&*s->declaredType);
                }
                collectTypeRefsInExpr(*s->value, refs);
            }
            else if (auto* s = dynamic_cast<ReturnStmt*>(stmt.get()))
            {
                if (s->value)
                {
                    collectTypeRefsInExpr(*s->value, refs);
                }
            }
            else if (auto* s = dynamic_cast<WhileStmt*>(stmt.get()))
            {
                collectTypeRefsInExpr(*s->condition, refs);
                collectTypeRefsInExpr(*s->body, refs);
            }
            else if (auto* s = dynamic_cast<BreakStmt*>(stmt.get()))
            {
                if (s->value)
                {
                    collectTypeRefsInExpr(*s->value, refs);
                }
            }
            else if (auto* s = dynamic_cast<ExprStmt*>(stmt.get()))
            {
                collectTypeRefsInExpr(*s->expr, refs);
            }
            else if (auto* s = dynamic_cast<FieldAssignStmt*>(stmt.get()))
            {
                collectTypeRefsInExpr(*s->object, refs);
                collectTypeRefsInExpr(*s->value, refs);
            }
            else if (auto* s = dynamic_cast<IndexAssignStmt*>(stmt.get()))
            {
                collectTypeRefsInExpr(*s->object, refs);
                collectTypeRefsInExpr(*s->index, refs);
                collectTypeRefsInExpr(*s->value, refs);
            }
            else if (auto* s = dynamic_cast<IncDecStmt*>(stmt.get()))
            {
                collectTypeRefsInExpr(*s->target, refs);
            }
            // ContinueStmt, and any declaration-shaped Stmt nested in a block (none exist in this
            // grammar today): nothing to collect.
        }
        if (block.result)
        {
            collectTypeRefsInExpr(*block.result, refs);
        }
    }

    void collectTypeRefsInExpr(Expr& expr, std::vector<std::string*>& refs)
    {
        if (auto* e = dynamic_cast<BinaryExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->left, refs);
            collectTypeRefsInExpr(*e->right, refs);
        }
        else if (auto* e = dynamic_cast<CastExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->operand, refs);
            refs.push_back(&e->targetType);
        }
        else if (auto* e = dynamic_cast<SomeExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->value, refs);
        }
        else if (auto* e = dynamic_cast<ShareExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->value, refs);
        }
        else if (auto* e = dynamic_cast<OkExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->value, refs);
        }
        else if (auto* e = dynamic_cast<ErrExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->value, refs);
        }
        else if (auto* e = dynamic_cast<TryExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->operand, refs);
        }
        else if (auto* e = dynamic_cast<InterpolatedStringExpr*>(&expr))
        {
            for (auto& piece : e->pieces)
            {
                if (piece.expr)
                {
                    collectTypeRefsInExpr(*piece.expr, refs);
                }
            }
        }
        else if (auto* e = dynamic_cast<IfExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->condition, refs);
            collectTypeRefsInExpr(*e->thenBranch, refs);
            if (e->elseBranch)
            {
                collectTypeRefsInExpr(*e->elseBranch, refs);
            }
        }
        else if (auto* e = dynamic_cast<MatchExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->scrutinee, refs);
            for (auto& arm : e->arms)
            {
                if (arm.body)
                {
                    collectTypeRefsInExpr(*arm.body, refs);
                }
            }
        }
        else if (auto* e = dynamic_cast<LoopExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->body, refs);
        }
        else if (auto* e = dynamic_cast<CallExpr*>(&expr))
        {
            for (auto& arg : e->arguments)
            {
                collectTypeRefsInExpr(*arg, refs);
            }
        }
        else if (auto* e = dynamic_cast<FieldExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->object, refs);
        }
        else if (auto* e = dynamic_cast<StructLiteralExpr*>(&expr))
        {
            refs.push_back(&e->typeName);
            for (auto& [fieldName, value] : e->fields)
            {
                collectTypeRefsInExpr(*value, refs);
            }
        }
        else if (auto* e = dynamic_cast<ArrayLiteralExpr*>(&expr))
        {
            for (auto& element : e->elements)
            {
                collectTypeRefsInExpr(*element, refs);
            }
        }
        else if (auto* e = dynamic_cast<IndexExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->object, refs);
            collectTypeRefsInExpr(*e->index, refs);
        }
        else if (auto* e = dynamic_cast<StrSliceExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->object, refs);
            if (e->start)
            {
                collectTypeRefsInExpr(*e->start, refs);
            }
            if (e->end)
            {
                collectTypeRefsInExpr(*e->end, refs);
            }
        }
        else if (auto* e = dynamic_cast<ListNewExpr*>(&expr))
        {
            refs.push_back(&e->elementType);
        }
        else if (auto* e = dynamic_cast<MethodCallExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->object, refs);
            for (auto& arg : e->arguments)
            {
                collectTypeRefsInExpr(*arg, refs);
            }
            if (!e->typeArgument.empty())
            {
                refs.push_back(&e->typeArgument);
            }
        }
        else if (auto* e = dynamic_cast<MapNewExpr*>(&expr))
        {
            refs.push_back(&e->keyType);
            refs.push_back(&e->valueType);
        }
        else if (auto* e = dynamic_cast<SortedMapNewExpr*>(&expr))
        {
            refs.push_back(&e->keyType);
            refs.push_back(&e->valueType);
        }
        else if (auto* e = dynamic_cast<SetNewExpr*>(&expr))
        {
            refs.push_back(&e->elementType);
        }
        else if (auto* e = dynamic_cast<StackNewExpr*>(&expr))
        {
            refs.push_back(&e->elementType);
        }
        else if (auto* e = dynamic_cast<LinkedListNewExpr*>(&expr))
        {
            refs.push_back(&e->elementType);
        }
        else if (auto* e = dynamic_cast<DequeNewExpr*>(&expr))
        {
            refs.push_back(&e->elementType);
        }
        else if (auto* e = dynamic_cast<QueueNewExpr*>(&expr))
        {
            refs.push_back(&e->elementType);
        }
        else if (auto* e = dynamic_cast<PriorityQueueNewExpr*>(&expr))
        {
            refs.push_back(&e->elementType);
        }
        else if (auto* e = dynamic_cast<SortedSetNewExpr*>(&expr))
        {
            refs.push_back(&e->elementType);
        }
        else if (auto* e = dynamic_cast<StringNewExpr*>(&expr))
        {
            collectTypeRefsInExpr(*e->text, refs);
        }
        else if (auto* e = dynamic_cast<BlockExpr*>(&expr))
        {
            collectTypeRefsInBlock(*e, refs);
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
            collectTypeRefsInExpr(*e->body, refs);
        }
        // IntegerExpr/Int64Expr/FloatExpr/NameExpr/NoneExpr/BoolExpr/StringExpr/CharExpr/
        // BufferNewExpr: leaves with no nested expression or type-string to collect.
    }

    // Every live std::string* that might hold a generic-instantiation reference, anywhere in the
    // program. StructDecl fields are collected only for a *concrete* struct (typeParams empty) -
    // a generic template's own fields legitimately contain an unsubstituted type-parameter
    // placeholder (e.g. "T", or "List<T>"), which must never be looked up as if it were a real
    // instantiation; only after GenericMonomorphizer substitutes a template's fields into a fresh
    // concrete synthesized StructDecl (added to program.items with an empty typeParams) do those
    // substituted field types become eligible for the next fixed-point iteration.
    std::vector<std::string*> collectGenericReferences(Program& program)
    {
        std::vector<std::string*> refs;
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
                for (auto& param : f->params)
                {
                    refs.push_back(&param.type);
                }
                if (f->returnType)
                {
                    refs.push_back(&*f->returnType);
                }
                collectTypeRefsInExpr(*f->body, refs);
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
                collectTypeRefsInExpr(*a->value, refs);
            }
            else if (auto* es = dynamic_cast<ExprStmt*>(item.get()))
            {
                collectTypeRefsInExpr(*es->expr, refs);
            }
            // TraitDecl/ImplDecl/EnumDecl/ModuleDecl/UseDecl: no generic-struct instantiation
            // reference can appear in any of these this milestone (no generic functions/methods
            // yet - see docs/language/0006-generics.md's own Non-Goals).
        }
        return refs;
    }
} // namespace

void monomorphizeGenerics(Program& program)
{
    std::unordered_map<std::string, StructDecl*> declsByName;
    for (auto& item : program.items)
    {
        if (auto* s = dynamic_cast<StructDecl*>(item.get()))
        {
            declsByName[s->name] = s;
        }
    }

    bool changed = true;
    while (changed)
    {
        changed = false;
        const std::vector<std::string*> refs = collectGenericReferences(program);

        for (std::string* ref : refs)
        {
            if (ref->find('<') == std::string::npos)
            {
                continue;
            }

            const auto [genericName, argStrings] = splitGenericInstantiation(*ref);
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

            const std::string mangled = mangleTypeText(*ref);
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
                changed = true;
            }

            *ref = mangled;
            changed = true;
        }
    }
}
