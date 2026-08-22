#pragma once

#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
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
struct LinkedListInstance;
struct DequeInstance;
struct QueueInstance;
struct PriorityQueueInstance;
struct MapInstance;
struct SetInstance;
struct SortedMapInstance;
struct SortedSetInstance;
struct StringInstance;
struct BufferInstance;
struct OptionalInstance;

using Value =
    std::variant<std::int64_t, // i32 and i64 both (see docs/language/0005-type-system.md) -
                               // this interpreter never distinguishes their width at runtime,
                               // only TypeChecker does, exactly like i32/char already share the
                               // numeric-unification `asInt` helper provides
                 double,       // f64 - the one numeric kind that needs a genuinely distinct C++
                               // type here, since integer and floating-point arithmetic are
                               // real, different operations, unlike i32-vs-i64's shared width
                 bool,
                 char32_t, // a single Unicode scalar value (see docs/language/0044-char.md) -
                           // a genuinely distinct C++ type from std::int64_t/bool, so it needs
                           // no wrapper struct the way BufferInstance/StringInstance do (those
                           // are reference-semantics; char, like i32/bool, is a plain value)
                 std::string,
                 std::shared_ptr<StructInstance>,
                 std::shared_ptr<ArrayInstance>,
                 std::shared_ptr<SliceInstance>,
                 std::shared_ptr<ListInstance>,
                 std::shared_ptr<StackInstance>,
                 std::shared_ptr<LinkedListInstance>,
                 std::shared_ptr<DequeInstance>,
                 std::shared_ptr<QueueInstance>,
                 std::shared_ptr<PriorityQueueInstance>,
                 std::shared_ptr<MapInstance>,
                 std::shared_ptr<SetInstance>,
                 std::shared_ptr<SortedMapInstance>,
                 std::shared_ptr<SortedSetInstance>,
                 std::shared_ptr<StringInstance>,
                 std::shared_ptr<BufferInstance>,
                 std::shared_ptr<OptionalInstance>,
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

// A doubly linked, node-based collection - see
// docs/language/0036-linked-lists.md. The interpreter's own representation
// doesn't need to mirror the LLVM backend's node-based layout (exactly like
// MapInstance/SetInstance below use std::unordered_map/std::unordered_set
// even though the compiled backend hand-rolls a chained hash table):
// std::deque gives O(1) push_front/push_back/pop_front/pop_back for free.
// pop_front/pop_back throw on empty (matching List<T>.pop()/Stack<T>.pop()'s
// own precedent).
struct LinkedListInstance
{
    std::deque<Value> elements;
};

// A growable array with a `start` offset - see docs/language/0037-deques.md.
// Unlike LinkedListInstance's own std::deque, this uses `std::vector<Value>`
// specifically so it plugs directly into asIndexable's existing
// `Indexable{std::vector<Value>*, length}` mechanism (Interpreter.cpp) -
// `.length`/`[i]`/`[i]=`/`for`-in all come for free that way, the same way
// Deque<T>'s real O(1)-indexing representation makes them cheap in the LLVM
// backend too. push_front/pop_front are O(n) here (insert/erase at
// `begin()`) - purely an interpreter representation choice, orthogonal to
// the LLVM backend's actual O(1) start-offset design, exactly like
// LinkedListInstance's own std::deque was already an interpreter-only
// choice. pop_front/pop_back throw on empty, matching precedent.
struct DequeInstance
{
    std::vector<Value> elements;
};

// A FIFO collection backed internally by Deque<T>'s own machinery - see
// docs/language/0038-queues.md. `std::deque`, not `std::vector` (unlike
// DequeInstance's own choice above): Queue<T> has no indexing to support,
// so there's no reason to plug into asIndexable, and std::deque gives real
// O(1) enqueue(push_back)/dequeue(pop_front) directly. dequeue() throws on
// empty, matching precedent.
struct QueueInstance
{
    std::deque<Value> elements;
};

// A real binary heap - see docs/language/0039-priority-queues.md. Unlike
// every earlier collection here, push/pop can't be a bare vector operation
// renamed: `Interpreter.cpp` runs the same std::push_heap/std::pop_heap
// sift-up/sift-down algorithm the LLVM backend hand-rolls, over a
// std::vector<Value> (plugging into the same STL heap algorithms List<T>'s
// own push/pop never needed). pop()/peek() throw on empty, matching every
// other collection's identical precedent.
struct PriorityQueueInstance
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

// Orderable, structural less-than over Value - for SortedMap<K,V>/
// SortedSet<T> below (see docs/language/0040-sorted-maps.md,
// docs/language/0041-sorted-sets.md), which need a real ordering, not just
// the Hash+Eq ValueHash/ValueEq above provide. Only ever invoked on the
// three kinds TypeChecker::isOrderableKind actually allows through - i32/
// char (unified numerically, exactly like `asInt` already unifies them for
// arithmetic/ordering elsewhere) and str (a bare std::string, compared by
// its own real `<`) - so, unlike ValueHash/ValueEq, this needs no struct/
// array/List recursion or self-reference guard; those types were never
// orderable to begin with.
struct ValueLess
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

// SortedMap<K,V> (see docs/language/0040-sorted-maps.md) - a real AVL tree
// in the compiled backend, but (mirroring MapInstance/SetInstance's own
// choice above, and LinkedListInstance's identical reasoning) the
// interpreter's own representation doesn't need to replicate that
// node-based layout: std::map<Value, Value, ValueLess> is *already* a real
// balanced tree (red-black, in every standard implementation), giving the
// exact same "keys stay ordered, O(log n) operations" behavior for free. K
// is a full Value (not a flat std::int64_t) precisely because K is no
// longer i32-only - it's any of the three kinds ValueLess above orders
// (i32, char, str; see TypeChecker::isOrderableKind and
// docs/language/0039-priority-queues.md).
struct SortedMapInstance
{
    std::map<Value, Value, ValueLess> entries;
};

// SortedSet<T> (see docs/language/0041-sorted-sets.md) - same reasoning as
// SortedMapInstance above: std::set<Value, ValueLess> is already a real
// balanced tree, giving "elements stay ordered" for free with zero
// hand-rolled AVL logic.
struct SortedSetInstance
{
    std::set<Value, ValueLess> elements;
};

// String (see docs/language/0042-string.md) - Axea's own owned, growable
// byte buffer, distinct from `str` (a bare std::string, not wrapped in a
// shared_ptr - see the Value variant above) precisely because String needs
// reference semantics: `.append()` mutates in place, so every existing
// alias must see the update, the same "stable pointer, mutated in place"
// model every List/Stack/Map/... instance here already follows. The LLVM
// backend's own header is byte-for-byte List<i8>'s shape; the interpreter
// makes no attempt to mirror that - std::string already *is* a real,
// growable byte buffer, so wrapping one directly is the natural
// equivalent (same "don't replicate the compiled backend's own internal
// layout" principle MapInstance/SortedMapInstance's own choices already
// established).
struct StringInstance
{
    std::string data;
};

// Buffer (see docs/language/0043-buffer.md) - mirrors StringInstance's own
// choice to wrap a real std::string directly rather than replicate the LLVM
// backend's own separate length/capacity/data layout: std::string already
// tracks its own capacity, so `.reserve()`/`.capacity` ride its existing
// growth machinery for free. `.capacity`'s *exact* numeric value is
// consequently expected to diverge from the compiled backend's own explicit
// doubling formula - an accepted, implementation-defined difference (see
// docs/language/0043-buffer.md's own Known Imprecision section), unlike
// `.length`/content/`.finish()`'s result, which must and do match exactly.
struct BufferInstance
{
    std::string data;
    // Tracks the compiled backend's own explicit doubling-growth capacity
    // (see ensureBufferCapacity/emitBufferNew/emitBufferAppend,
    // docs/language/0043-buffer.md) - *not* `data.capacity()`. Those two
    // are genuinely different quantities: `std::string::capacity()` is
    // libstdc++'s own implementation-defined growth/SSO threshold (15 on
    // a typical 64-bit build, regardless of what was actually appended,
    // since a short string fits entirely in the small-string-optimization
    // inline buffer) - it was never a correct stand-in for "how many
    // bytes this language's own Buffer.capacity would report" and only
    // happened to go unnoticed until a real byte-for-byte comparison
    // against the compiled backend's own hand-rolled algorithm was run.
    std::int64_t capacity = 1;
};

// Reference semantics (shared_ptr), mirroring every other collection above
// - see docs/language/0052-optional.md. `value` holds a default-constructed
// Value (std::monostate) when `hasValue` is false; never read in that state
// (every consumer - `?`, .unwrap_or/.is_some/.is_none - checks `hasValue`
// first).
struct OptionalInstance
{
    bool hasValue;
    Value value;
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
    // extern c functions (see docs/language/0048-ffi.md) - a small,
    // explicit allowlist of hand-implemented libc functions (currently
    // just "puts"), since the interpreter can't dynamically link against
    // arbitrary C symbols the way the compiled backend's own `declare`+
    // `call` genuinely does. Any other name throws.
    Value callExtern(const std::string& name, const std::vector<Value>& args);

    std::unordered_map<std::string, const FunctionDecl*> functions_;
    std::unordered_map<std::string, const StructDecl*> structs_;
    Environment globalEnv_;
};
