#pragma once

#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Matches docs/language/0005-type-system.md's "Initial Type Checker
// Representation" and 0002-grammar.md's primitive_type list. Only Bool, I32,
// I64/F64 (docs/language/0051-numeric-widening.md), Char
// (docs/language/0044-char.md), String, Unit, Struct, Array
// (docs/language/0031-arrays.md), Slice (docs/language/0032-slices.md), List
// (docs/language/0033-lists.md), Map/Set (docs/language/0034-maps-and-sets.md),
// Stack (docs/language/0035-stacks.md), LinkedList
// (docs/language/0036-linked-lists.md), Deque
// (docs/language/0037-deques.md), Queue (docs/language/0038-queues.md),
// PriorityQueue/SortedMap/SortedSet (docs/language/0039-priority-queues.md,
// 0040-sorted-maps.md, 0041-sorted-sets.md - element/key type restricted to
// isOrderableKind: I32/I64/F64/Char/String), and OwnedString
// (docs/language/0042-string.md - Axea's own `String`, distinct from this
// same enum's `String` case which is `str`) have checking logic wired up
// this phase; the rest (I8/I16/I128, every unsigned width, F32) are
// declared for architectural fidelity and reachable only as an "unsupported
// type" error via resolveType.
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
    // `Result<T,E>` (see docs/language/0063-result.md) - shares `Type`'s
    // existing `elementTypeName`/`valueTypeName` fields with Map<K,V>
    // rather than getting its own: elementTypeName is T (the Ok payload,
    // same role Optional<T> already gives that field), valueTypeName is E
    // (the Err payload) - genuinely the same "two independent type
    // parameters, string-keyed, arbitrarily nestable" shape Map<K,V>
    // already established, just with neither parameter under a
    // hashability/orderability constraint the way Map's own K is (see
    // Type's own field comment below). Deliberately a new enumerator, not
    // a reuse of the pre-existing (and, before this phase, completely
    // inert - zero references anywhere in this file) TypeKind::Error just
    // below: that enumerator's own original intent is undocumented and
    // unrelated to this phase, so repurposing it would be presumptuous
    // rather than principled.
    Result,
    Error,
    // A user-declared `enum` (see docs/language/0064-enums.md) - a genuine tagged union/
    // algebraic data type. Reuses `Type::structName` to carry the enum's own name, the same
    // field `Struct` just below already uses for its own name - variant payload types live on
    // the registered `EnumDecl` itself (looked up by that name), not duplicated into `Type`.
    Enum,
    // `fn(T1,T2)->R` (see docs/language/0067-closures.md) - a closure value's own type. Reuses
    // `Type::structName` to carry its own canonical signature string, the same "structName IS my
    // own identity" role Struct/Enum already give that field (a closure's *own* signature is its
    // identity, unlike Optional<T>'s "structName unused, elementTypeName is my nested payload"
    // shape) - re-split back into (param types, return type) on demand via
    // closureParamAndReturnTypes wherever the structured form is actually needed (mirrors
    // Result<T,E>'s own "store the canonical string, re-resolve on demand" convention).
    Closure,
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
    Buffer,
    // `cstr` (see docs/language/0048-ffi.md) - representationally
    // identical to `str` (both a bare, null-terminated i8*), but kept a
    // genuinely distinct TypeKind rather than an alias for `String`
    // (str's own kind): `docs/std/strings/0007-ffi.md`'s own design
    // requires an explicit `.to_cstr()` conversion rather than silent
    // interchangeability, mirroring the deliberate one-way "String lends
    // a str" coercion rule rather than treating the two as equal.
    CStr
};

struct Type
{
    TypeKind kind;
    std::string structName; // populated only when kind == TypeKind::Struct

    // Populated only when kind == TypeKind::Array (see docs/language/0031-arrays.md)
    // - a compile-time-known element count, genuinely distinct from any
    // type parameter, so it stays its own field even though Array now
    // shares elementTypeName below with every other single-type-parameter
    // kind.
    int arraySize{};

    // The single type parameter for every kind that has exactly one -
    // Array/Slice/List/Stack/LinkedList/Deque/Queue/PriorityQueue/Optional/
    // Set, plus Map/SortedMap's own key (see docs/language/0034-maps-and-sets.md's
    // generic rewrite) and Result<T,E>'s own Ok payload (T - see
    // docs/language/0063-result.md). Genuinely arbitrarily nested (List<Optional<List<i32>>>,
    // a struct, another Map<...>, ...) - a single TypeKind can't carry that
    // (this field used to be a flat elementKind/elementStructName pair,
    // exactly that limitation, until docs/language/0052-optional.md's own
    // follow-up found it silently corrupting List<Optional<i32>>), so this
    // stores the canonical resolveType-able string instead, re-resolved on
    // demand via resolveType(...) wherever the full nested Type is
    // actually needed (mirrors MapNewExpr/SetNewExpr's own "store the
    // string, re-resolve later" pattern at the AST layer). valueTypeName is
    // Map/SortedMap's own value type, or Result<T,E>'s own Err payload (E -
    // empty for every single-parameter kind, including Set). Always stored
    // in canonical form (typeName(...)
    // of the resolved element, not raw source text) so the defaulted
    // operator== below still works structurally.
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
    // Closures (see docs/language/0067-closures.md) - checkExpr's own CallExpr case needs to
    // know "is `callee` a bound local/param at all" *before* deciding whether it's a closure
    // call, without get()'s own "throw if not found" behavior forcing a try/catch for the
    // ordinary case (an actual top-level function name, never bound in any TypeEnv).
    bool contains(const std::string& name) const;

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
    // Returns the enum `objectExpr` names, if it's a bare NameExpr matching a registered enum
    // type - the shared "is this actually EnumName.Variant(...)/EnumName.Variant construction,
    // not a real method/field access" check both checkExpr's MethodCallExpr and FieldExpr cases
    // need (see docs/language/0064-enums.md). Checked *before* either case's own generic
    // checkExpr(*object,...)/checkFieldType call, which would otherwise throw "undefined
    // variable" trying to resolve a bare enum type name as if it were bound to a value.
    const EnumDecl* asEnumTypeName(const Expr& objectExpr) const;
    // "T1 | T2 | ..." anonymous union types (see docs/language/0065-unions.md) - lower to the
    // exact same enum-as-flattened-struct machinery `enum` already uses, just auto-registered
    // on demand instead of user-declared: the first time a given canonical ("|"-joined, sorted,
    // deduplicated - see Parser::parseTypeName) union string is seen, a synthetic EnumDecl is
    // built (one variant per alternative, named after that alternative's own canonical type
    // name, with that one type as its single field) and memoized into enums_ under the union's
    // own canonical string as its "name" - every later pass (match, printing, construction) then
    // treats it exactly like a real declared enum, with zero extra dispatch logic anywhere.
    // `const` despite mutating enums_/unionDecls_: mirrors Map<K,V>/Optional<T>'s own
    // auto-grows-on-demand instantiation caches, which every existing const-method caller here
    // already treats as pure lookups.
    Type resolveUnionType(const std::string& canonicalName) const;
    // True if `valueType` is assignable to `targetType` by implicit union wrapping - `targetType`
    // is a union (its enums_ entry's own canonical name, i.e. containing '|') and `valueType`
    // matches exactly one of its alternatives. Used wherever plain Type equality currently gates
    // assignment/argument/return (mirrors arrayToSliceCoercion/stringToStrCoercion's own
    // "named coercion predicate OR'd into the equality check" shape at each such site) - the
    // user-facing point of TypeScript-style unions (`f(5)`/`f("hi")` for `f(x: i32 | str)`, no
    // wrapper syntax needed).
    bool isUnionMember(const Type& valueType, const Type& targetType) const;
    // Shared arg-count/per-arg-type checking (with the array->slice/String->str/union-wrap
    // coercions - see each one's own comment) for a resolved callee's (params, returnType) -
    // originally CallExpr's own inline logic, factored out so a module-qualified call
    // (`math.sqrt(x)` - see docs/language/0066-modules.md) can reuse it verbatim instead of
    // duplicating it.
    Type checkCallArguments(const std::string& calleeDisplayName,
                            const std::vector<Param>& params,
                            const std::optional<std::string>& returnType,
                            const std::vector<std::unique_ptr<Expr>>& arguments,
                            TypeEnv& env,
                            const Type* expectedReturnType,
                            std::vector<Type>* currentLoopBreakTypes);
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
    // `enum` declarations (see docs/language/0064-enums.md) - a parallel map to structs_ above,
    // rather than folded into it: an EnumDecl is a genuinely different AST node (variants, not
    // fields), even though a resolved Enum Type reuses Struct's own `structName` field to carry
    // the enum's own name (see resolveType's own "Name" branch).
    // mutable: resolveUnionType (called from the const resolveType) auto-registers a synthetic
    // union's EnumDecl into this map the first time its canonical name is seen (see
    // resolveUnionType's own comment) - real user-declared enums are inserted non-const, in
    // registerSignatures, exactly as before.
    mutable std::unordered_map<std::string, const EnumDecl*> enums_;
    // Owns every synthetic union EnumDecl resolveUnionType builds, so the raw pointers stored
    // into enums_ above stay valid for the checker's whole lifetime (a real declared EnumDecl's
    // pointer is instead owned by Program::items, which already outlives the checker).
    mutable std::vector<std::unique_ptr<EnumDecl>> unionDecls_;
    // extern c function declarations (see docs/language/0048-ffi.md) - a
    // parallel map to functions_ above rather than folded into it: an
    // ExternDecl is a genuinely different AST node (no body), even though
    // its own params/returnType shape happens to match FunctionDecl's.
    // CallExpr's own type-checking consults both.
    std::unordered_map<std::string, const ExternDecl*> externs_;
    // trait/impl (see docs/language/0062-display-trait.md) - traits_ is
    // consulted only for a real, if minimal (name+arity only, not full
    // per-parameter type conformance) `impl` conformance check;
    // registerSignatures folds every impl method straight into
    // functions_ above (mangled name, see ImplDecl's own comment), so
    // TypeChecker's later per-function validation pass needs no
    // separate impl-aware code path at all.
    std::unordered_map<std::string, const TraitDecl*> traits_;
    // Modules (see docs/language/0066-modules.md) - the set of real module names self-derived
    // from functions_/externs_'s own already-'.'-qualified keys (see registerSignatures's own
    // comment). Consulted by checkExpr's MethodCallExpr case: `object` being a bare NameExpr
    // matching this set means "math.sqrt(...)" is a qualified module call, not a real method
    // call, resolved directly against functions_["math.sqrt"]/externs_["math.sqrt"] instead of
    // the generic checkExpr(*methodCall->object,...) path (mirrors asEnumTypeName's identical
    // "is object a bare name matching something known, checked before generic resolution"
    // shape).
    std::unordered_set<std::string> moduleNames_;
    // The enclosing function's own module, if any (the substring before functions_'s own last
    // '.' in its qualified name - "" for a root-file function). Set once, at the top of
    // checkFunction, and left untouched through that whole function body's own recursive
    // checkExpr/checkStmt/checkBlock traversal (never reentrant - this codebase has no nested
    // function declarations/closures, so checkFunction is never called again from within an
    // outer checkFunction's own recursion) - a single ambient member field instead of a new
    // parameter threaded through every one of those signatures. Consulted only to exempt a
    // module's own qualified self-reference (`math.helper()` called from within math's own code)
    // from the `pub` check a genuinely external qualified reference needs.
    std::string currentFunctionModule_;
};
