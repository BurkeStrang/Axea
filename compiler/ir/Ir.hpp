#pragma once

#include "lexer/TokenKind.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// A structured (not flattened to a basic-block CFG) intermediate
// representation, lowered from the fully checked AST. See
// docs/language/0021-axea-ir.md for the design rationale - in particular why
// control flow stays structured (one Branch instruction holding two nested
// instruction lists) instead of basic blocks and phi nodes, and why
// ownership/capability/region information is embedded directly in the
// instruction stream (BorrowRead/BorrowWrite/Move/RegionEnter/RegionExit/Drop)
// rather than being generic three-address code.
struct IrInst
{
    virtual ~IrInst() = default;
    int dest = -1; // virtual register this instruction defines; -1 if none
};

struct IrConstInt final : IrInst
{
    std::int64_t value;
};

struct IrConstBool final : IrInst
{
    bool value;
};

struct IrConstString final : IrInst
{
    std::string value;
};

struct IrBinOp final : IrInst
{
    TokenKind op;
    int lhs;
    int rhs;
};

struct IrCall final : IrInst
{
    std::string callee;
    std::vector<int> args;
};

struct IrStructNew final : IrInst
{
    std::string typeName;
    std::vector<std::pair<std::string, int>> fields;
};

struct IrFieldGet final : IrInst
{
    int object;
    std::string field;
};

struct IrFieldSet final : IrInst
{
    int object;
    std::string field;
    int value;
};

// `[e1, e2, ...]`. No element-type field: every element register's LLVM type
// is already inferred by the time this is reached (elements are always
// lowered, and therefore type-inferred, before the IrArrayNew instruction
// that references them), so LlvmIrEmitter derives the array's element type
// from elements.front() rather than needing it carried here - see
// docs/language/0031-arrays.md.
struct IrArrayNew final : IrInst
{
    std::vector<int> elements;
};

struct IrIndexGet final : IrInst
{
    int object;
    int index;
};

struct IrIndexSet final : IrInst
{
    int object;
    int index;
    int value;
};

// `List<elem>()` - a fresh, empty, growable list (see docs/language/0033-lists.md).
// `elementTypeName` is carried explicitly (unlike IrArrayNew, which infers it
// from its own elements) - a brand-new empty list has no elements to infer
// the type from.
struct IrListNew final : IrInst
{
    std::string elementTypeName;
};

// `list.push(value)` - no dest (void); mutates `list`'s own header fields in
// place. IndexGet/IndexSet/FieldGet(".length") are all reused unchanged for
// List (see docs/language/0033-lists.md) - only push/pop need dedicated
// instructions, since they're the only operations that change a list's shape
// rather than just reading/writing an existing slot.
struct IrListPush final : IrInst
{
    int list;
    int value;
};

// `list.pop()` - dest is the removed element.
struct IrListPop final : IrInst
{
    int list;
};

// `Stack<T>()` - a LIFO collection backed internally by List<T>'s own
// machinery (see docs/language/0035-stacks.md). Carries elementTypeName
// exactly like IrListNew - a brand-new empty stack has nothing to infer it
// from.
struct IrStackNew final : IrInst
{
    std::string elementTypeName;
};

// `stack.push(value)` - no dest (void); mirrors IrListPush exactly.
struct IrStackPush final : IrInst
{
    int stack;
    int value;
};

// `stack.pop()` - dest is the removed element; mirrors IrListPop exactly.
struct IrStackPop final : IrInst
{
    int stack;
};

// `stack.peek()` - dest is the top element, *not* removed (the one
// genuinely new operation List<T> doesn't have - see
// docs/language/0035-stacks.md).
struct IrStackPeek final : IrInst
{
    int stack;
};

// `Map<K,V>()` - a fresh, empty hash table (see docs/language/0034-maps-and-sets.md's
// generic rewrite). Carries the concrete K/V type strings explicitly - like
// IrListNew's own elementTypeName, a brand-new empty Map has nothing to infer
// them from - so LlvmIrEmitter can look up (or register, on first sight) the
// right monomorphized instantiation.
struct IrMapNew final : IrInst
{
    std::string keyTypeName;
    std::string valueTypeName;
};

// `map.set(key, value)` - no dest (unit); inserts or updates in place.
struct IrMapSet final : IrInst
{
    int map;
    int key;
    int value;
};

// `map.get(key)` - dest is the value (or an unspecified sentinel if the key
// is absent in compiled code; the interpreter throws instead - see
// docs/language/0034-maps-and-sets.md).
struct IrMapGet final : IrInst
{
    int map;
    int key;
};

// `map.contains(key)` - dest is a bool.
struct IrMapContains final : IrInst
{
    int map;
    int key;
};

// `map.remove(key)` - no dest (unit); no-op if the key is absent.
struct IrMapRemove final : IrInst
{
    int map;
    int key;
};

// `Set<T>()` - a fresh, empty hash set. Same reasoning as IrMapNew above.
struct IrSetNew final : IrInst
{
    std::string elementTypeName;
};

// `set.add(value)` - no dest (unit); no-op if already present.
struct IrSetAdd final : IrInst
{
    int set;
    int value;
};

// `set.contains(value)` - dest is a bool.
struct IrSetContains final : IrInst
{
    int set;
    int value;
};

// `set.remove(value)` - no dest (unit); no-op if absent.
struct IrSetRemove final : IrInst
{
    int set;
    int value;
};

// `if`/`else`, kept structured: two nested instruction lists rather than
// separate labeled blocks, since the language has no loops yet and this
// avoids needing real CFG merging/phi nodes for something nothing downstream
// consumes yet. `dest` (from IrInst) is the merge register; `thenValue`/
// `elseValue` name which register within each nested list actually holds
// the branch's result (-1 means that branch produces unit).
struct IrBranch final : IrInst
{
    int condition;
    std::vector<std::unique_ptr<IrInst>> thenBlock;
    std::vector<std::unique_ptr<IrInst>> elseBlock;
    int thenValue = -1;
    int elseValue = -1;
};

struct IrReturn final : IrInst
{
    int value = -1; // -1 => bare/unit return
};

// One of these is emitted per parameter at function entry, chosen from the
// parameter's already-resolved capability/region (CapabilityChecker /
// RegionChecker) - not recomputed here.
struct IrBorrowRead final : IrInst
{
    int value;
};

struct IrBorrowWrite final : IrInst
{
    int value;
};

struct IrMove final : IrInst
{
    int value;
};

struct IrRegionEnter final : IrInst
{
};

struct IrRegionExit final : IrInst
{
};

// A struct-typed local at its owning block's exit, or an owned (`take`)
// struct-typed parameter at function exit. Not move-aware: a value that was
// itself taken/moved elsewhere still gets a Drop marker here (documented
// limitation - see docs/language/0021-axea-ir.md).
struct IrDrop final : IrInst
{
    int value;
};

// `while`/`loop`, kept structured like IrBranch: a nested conditionBlock
// (empty for infinite `loop`) and body instead of separate labeled blocks.
// `dest` (from IrInst) is the loop's own produced value - only meaningful
// for `loop` (used when consumed as an expression); `while` never produces
// one. See docs/language/0028-loops.md for the full design, in particular
// why loop-carried mutation is represented via `carried` (consumed by the
// LLVM backend as alloca/load/store, not phi nodes) rather than anything
// resembling strict SSA at this level.
struct IrLoop final : IrInst
{
    std::vector<std::unique_ptr<IrInst>> conditionBlock; // empty for infinite `loop`
    int conditionValue = -1; // register in conditionBlock; -1 = infinite
    std::vector<std::unique_ptr<IrInst>> body;
    // (register holding a name's value just before the loop, register
    // holding it at the end of one static body traversal) per name mutated
    // inside the loop body - populated by diffing an IrScope snapshot taken
    // before/after lowering body.
    std::vector<std::pair<int, int>> carried;
};

// `break [value]`, always targets the innermost enclosing IrLoop. `carried`
// mirrors IrLoop::carried but snapshotted at *this* point in the body rather
// than the body's natural end - a break can fire before any/all
// reassignments happen, so the LLVM backend needs to know exactly which
// carried variables changed (and to what) by the time control reaches here,
// to correctly update their storage before jumping to the loop's exit.
struct IrBreak final : IrInst
{
    int value = -1; // -1 = bare `break`
    std::vector<std::pair<int, int>> carried;
};

// `continue`, always targets the innermost enclosing IrLoop. `carried`: see
// IrBreak - same reasoning, needed before jumping back to the loop header.
struct IrContinue final : IrInst
{
    std::vector<std::pair<int, int>> carried;
};

struct IrFunction
{
    std::string name;
    std::vector<std::string> paramNames;
    std::vector<std::string> paramTypes; // declared type names, parallel to paramNames
    std::optional<std::string> returnType;
    std::vector<std::unique_ptr<IrInst>> body;
    int registerCount = 0;
};

struct IrProgram
{
    std::vector<IrFunction> functions;
    std::vector<std::unique_ptr<IrInst>> topLevel;
    // (name, final register) for each top-level assignment, in source order -
    // mirrors how `ax run` reports top-level bindings, and is what a
    // generated `main` (LlvmIrEmitter) prints.
    std::vector<std::pair<std::string, int>> topLevelBindings;
    // struct name -> its fields, in declared order, as (fieldName, fieldType) pairs.
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> structs;
};
