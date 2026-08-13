#pragma once

#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"

#include <string>
#include <unordered_map>

// Matches docs/language/0005-type-system.md's "Initial Type Checker
// Representation" and 0002-grammar.md's primitive_type list. Only Bool, I32,
// String, Unit, and Struct have checking logic wired up this phase; the rest
// are declared for architectural fidelity and reachable only as an
// "unsupported type" error via resolveType.
enum class TypeKind
{
    Bool,

    I8,
    I16,
    I32,
    I64,
    I128,

    U8,
    U16,
    U32,
    U64,
    U128,

    F32,
    F64,

    Char,
    String,

    Unit,
    Never,

    Optional,
    Error,
    Struct,
    Tuple,
    Function,
    Generic,
    Reference,
    Pointer,
    Slice
};

struct Type
{
    TypeKind kind;
    std::string structName; // populated only when kind == TypeKind::Struct

    bool operator==(const Type&) const = default;
};

std::string typeName(const Type& type);

class TypeEnv
{
public:
    explicit TypeEnv(const TypeEnv* parent = nullptr);

    void define(const std::string& name, Type type);
    Type get(const std::string& name) const;

private:
    std::unordered_map<std::string, Type> types_;
    const TypeEnv* parent_;
};

class TypeChecker
{
public:
    void check(const Program& program);

private:
    void registerSignatures(const Program& program);
    void checkFunction(const FunctionDecl& function);
    // True if every path through the block hits an explicit `return` - a
    // top-level `return`, or an if/else where both branches (recursively)
    // definitely return. Mirrors how parseBlock decides ExprStmt vs. result
    // purely by "is this immediately followed by '}'".
    bool definitelyReturns(const BlockExpr& block) const;
    bool definitelyReturnsBranch(const IfExpr& ifExpr) const;
    Type checkBlock(const BlockExpr& block, TypeEnv& parentEnv, const Type* expectedReturnType);
    Type checkExpr(const Expr& expr, TypeEnv& env, const Type* expectedReturnType);
    void checkStmt(const Stmt& stmt, TypeEnv& env, const Type* expectedReturnType);
    Type checkFieldType(const Expr& object,
                        const std::string& field,
                        TypeEnv& env,
                        const Type* expectedReturnType);
    Type resolveType(const std::string& name) const;

    std::unordered_map<std::string, const FunctionDecl*> functions_;
    std::unordered_map<std::string, const StructDecl*> structs_;
};
