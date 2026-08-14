#pragma once

#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

struct StructInstance;
struct ArrayInstance;
struct SliceInstance;
struct ListInstance;
struct StackInstance;
struct MapInstance;
struct SetInstance;

using Value = std::variant<std::int64_t,
                           bool,
                           std::string,
                           std::shared_ptr<StructInstance>,
                           std::shared_ptr<ArrayInstance>,
                           std::shared_ptr<SliceInstance>,
                           std::shared_ptr<ListInstance>,
                           std::shared_ptr<StackInstance>,
                           std::shared_ptr<MapInstance>,
                           std::shared_ptr<SetInstance>,
                           std::monostate>;

struct StructInstance
{
    std::string typeName;
    std::vector<std::pair<std::string, Value>>
        fields; // declared field order, for deterministic printing
};

// Reference semantics (always accessed via shared_ptr), mirroring
// StructInstance - see docs/language/0031-arrays.md.
struct ArrayInstance
{
    std::vector<Value> elements;
};

// A non-owning view over an existing ArrayInstance - "pointer + length, but
// safely typed" (docs/language/0005-type-system.md §45; implementation in
// docs/language/0032-slices.md). `length` is stored explicitly rather than
// read off `backing->elements.size()`, to keep the "runtime, not
// compile-time" distinction real even though the two always agree in this
// whole-array-only-conversion scope (no sub-slicing yet).
struct SliceInstance
{
    std::shared_ptr<ArrayInstance> backing;
    std::size_t length;
};

// Reference semantics (shared_ptr), mirroring StructInstance/ArrayInstance -
// see docs/language/0033-lists.md. std::vector already provides real
// amortized growth internally; the LLVM backend is the only place growth
// needs to be hand-rolled (nothing here needs to know that).
struct ListInstance
{
    std::vector<Value> elements;
};

// A LIFO collection backed internally by List<T>'s own machinery - see
// docs/language/0035-stacks.md. Byte-identical to ListInstance; the
// distinction is purely at the Axea type-system level (Stack<T> isn't
// assignable to a List<T> parameter), not in how the interpreter represents
// it. push/pop are push_back/pop_back (pop throws on empty, matching
// List<T>.pop()); peek reads .back() without removing (throws on empty too,
// for the same reason).
struct StackInstance
{
    std::vector<Value> elements;
};

// Structural hash/equality over Value, for Map<K,V>/Set<T> (see
// docs/language/0034-maps-and-sets.md's generic rewrite - any type valid per
// TypeChecker::isHashable, not just i32). Unlike LlvmIrEmitter, which
// monomorphizes a distinct hash/equality implementation per concrete key
// shape, this needs only one: std::variant's own index() plus a recursive
// dispatch on whichever alternative is active. shared_ptr<StructInstance>/
// ArrayInstance/ListInstance hash/compare their *contents* (dereferenced),
// not pointer identity - two separately-constructed-but-equal keys must
// collide/compare equal, matching the value semantics the LLVM backend
// enforces. No cycle protection needed: a self-referential struct was
// already rejected as a key type by TypeChecker::isHashable before any
// Value of that shape could reach a Map/Set in a well-typed program.
struct ValueHash
{
    std::size_t operator()(const Value& value) const;
};

struct ValueEq
{
    bool operator()(const Value& a, const Value& b) const;
};

// Map<K,V>/Set<T> (see docs/language/0034-maps-and-sets.md's generic
// rewrite). The STL already provides a real, correct hash table for any key
// type once given hash/equality - ValueHash/ValueEq above are the only piece
// hand-rolled here; unlike LlvmIrEmitter, nothing about chaining/resize
// needs reimplementing.
struct MapInstance
{
    std::unordered_map<Value, Value, ValueHash, ValueEq> entries;
};

struct SetInstance
{
    std::unordered_set<Value, ValueHash, ValueEq> elements;
};

std::string toString(const Value& value);

class Environment
{
public:
    explicit Environment(Environment* parent = nullptr);

    void define(const std::string& name, Value value); // declares/shadows in this scope
    void assign(const std::string& name, Value value); // mutates an existing binding, walking up
    Value get(const std::string& name) const;
    bool
    contains(const std::string& name) const; // walks the chain; used to decide define vs assign

    const std::unordered_map<std::string, Value>& bindings() const;

private:
    std::unordered_map<std::string, Value> values_;
    Environment* parent_;
};

class Interpreter
{
public:
    void run(const Program& program);

    Value evaluate(const Expr& expr, Environment& env);
    void execute(const Stmt& stmt, Environment& env);

    const std::unordered_map<std::string, Value>& variables() const;

private:
    Value callFunction(const FunctionDecl& decl, std::vector<Value> args);

    std::unordered_map<std::string, const FunctionDecl*> functions_;
    std::unordered_map<std::string, const StructDecl*> structs_;
    Environment globalEnv_;
};
