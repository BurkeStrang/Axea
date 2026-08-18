#pragma once

#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Matches docs/language/0005-type-system.md's "Initial Type Checker
// Representation" and 0002-grammar.md's primitive_type list. Only Bool, I32,
// String, Unit, Struct, Array (docs/language/0031-arrays.md), Slice
// (docs/language/0032-slices.md), List (docs/language/0033-lists.md),
// Map/Set (docs/language/0034-maps-and-sets.md), Stack
// (docs/language/0035-stacks.md), LinkedList
// (docs/language/0036-linked-lists.md), Deque
// (docs/language/0037-deques.md), Queue (docs/language/0038-queues.md), and
// PriorityQueue (docs/language/0039-priority-queues.md, i32 elements only),
// and OwnedString (docs/language/0042-string.md - Axea's own `String`,
// distinct from this same enum's `String` case which is `str`) have
// checking logic wired up this phase; the rest are declared for
// architectural fidelity and reachable only as an "unsupported type" error
// via resolveType.
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
    Slice,
    Array,
    List,
    Map,
    Set,
    Stack,
    LinkedList,
    Deque,
    Queue,
    PriorityQueue,
    SortedMap,
    SortedSet,
    OwnedString,
    Buffer
};

struct Type
{
    TypeKind kind;
    std::string structName; // populated only when kind == TypeKind::Struct

    // Populated only when kind == TypeKind::Array (see docs/language/0031-arrays.md).
    // Flat, not recursive - no nested array types in this phase. Deliberately
    // not a std::shared_ptr<Type> element: that would make the defaulted
    // operator== below compare pointer identity instead of structural
    // equality, silently breaking every array-type comparison.
    TypeKind elementKind{};
    std::string elementStructName{};
    int arraySize{};

    // Map<K,V>/Set<T> only (see docs/language/0034-maps-and-sets.md's
    // generic rewrite). Unlike Array's flat elementKind tag above, K/V can
    // themselves be arbitrarily nested (List<i32>, a struct, another
    // Map<...>) - a single TypeKind can't carry that, so these store the
    // canonical resolveType-able string instead, re-resolved on demand via
    // resolveType(...) wherever the full nested Type is actually needed
    // (mirrors MapNewExpr/SetNewExpr's own "store the string, re-resolve
    // later" pattern at the AST layer). elementTypeName doubles as Set's own
    // element type; valueTypeName is Map-only (empty for Set). Always stored
    // in canonical form so the defaulted operator== below still works.
    std::string elementTypeName;
    std::string valueTypeName;

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
    // currentLoopBreakTypes: null when not inside a loop (rejects break/continue,
    // mirroring how a null expectedReturnType rejects `return` outside a
    // function); otherwise the active innermost loop's collector of every
    // `break value`'s type, used to type LoopExpr itself.
    Type checkBlock(const BlockExpr& block,
                    TypeEnv& parentEnv,
                    const Type* expectedReturnType,
                    std::vector<Type>* currentLoopBreakTypes);
    Type checkExpr(const Expr& expr,
                   TypeEnv& env,
                   const Type* expectedReturnType,
                   std::vector<Type>* currentLoopBreakTypes);
    void checkStmt(const Stmt& stmt,
                   TypeEnv& env,
                   const Type* expectedReturnType,
                   std::vector<Type>* currentLoopBreakTypes);
    Type checkFieldType(const Expr& object,
                        const std::string& field,
                        TypeEnv& env,
                        const Type* expectedReturnType,
                        std::vector<Type>* currentLoopBreakTypes);
    Type resolveType(const std::string& name) const;
    // True if `type` is a valid Map/Set key type: i32/bool/str always;
    // Array/List if their element is (recursing on the existing flat
    // elementKind/elementStructName representation); struct if every
    // declared field's resolved type is (recursing with the struct's own
    // name added to `visitedStructs` first, so a self-/mutually-recursive
    // struct chain - already possible today, since structs are always
    // by-pointer - short-circuits to false on revisit instead of looping
    // forever); Map/Set/slice/anything else: false (mirrors Rust: HashMap/
    // HashSet don't implement Hash - no canonical order to hash over). See
    // docs/language/0034-maps-and-sets.md.
    bool isHashable(const Type& type, std::unordered_set<std::string>& visitedStructs) const;

    std::unordered_map<std::string, const FunctionDecl*> functions_;
    std::unordered_map<std::string, const StructDecl*> structs_;
};
