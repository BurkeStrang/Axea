#pragma once

#include "ast/Stmt.hpp"
#include "ir/Ir.hpp"
#include "sema/RegionChecker.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Scope-chain for name -> virtual-register lookups during lowering, mirroring
// Environment's define()/assign() split (Interpreter.hpp): plain assignment
// shadows within its own block, while `++`/`--` on a bare name mutates the
// existing binding up the chain.
//
// `isBarrier` matters specifically for `assign()`: an if/else branch's scope
// is a barrier, because unlike the interpreter (which only ever actually
// executes one branch), lowering visits *both* branches structurally. If
// `assign()` were allowed to walk past a branch's own scope, mutating a name
// inside the then-branch would corrupt what the else-branch (lowered right
// after, against the same shared parent scope) sees - and would incorrectly
// persist past the `if` entirely, even though nothing merges the two
// branches' outcomes (no phi nodes - see docs/language/0021-axea-ir.md). A
// barrier scope still allows `assign()` to define locally (so later code
// within the *same* branch sees the update), it just refuses to reach past
// itself into the shared parent.
class IrScope
{
public:
    explicit IrScope(IrScope* parent = nullptr, bool isBarrier = false);

    void define(const std::string& name, int registerId);
    void assign(const std::string& name, int registerId);
    int find(const std::string& name) const;
    bool
    contains(const std::string& name) const; // walks the chain; used to decide define vs assign
    // Every name visible from here, walking the whole parent chain - used to
    // detect loop-carried variables (diff a snapshot taken before/after
    // lowering a loop body). Barrier-oblivious, like find()/contains().
    std::unordered_map<std::string, int> snapshot() const;

    // Parallel to the register scope above, but for a name's statically-known
    // array size (used only to constant-fold `.length` - see
    // IrGenerator::arrayLengthOf and docs/language/0031-arrays.md). Simple
    // define-shadows/lookup-walks-chain semantics, same as find(); no
    // assign()/barrier distinction needed since this is read-only metadata,
    // never mutated after a name is bound.
    void defineArrayLength(const std::string& name, int length);
    std::optional<int> findArrayLength(const std::string& name) const;

    // Parallel to the array-length map above, but records whether a
    // Map<i32,i32>/Set<i32>-typed name is specifically a Set (true) or a Map
    // (false) - needed only to disambiguate `.contains`/`.remove`, the two
    // method names shared between Map and Set (see
    // docs/language/0034-maps-and-sets.md and IrGenerator::isSetExpr).
    void defineIsSet(const std::string& name, bool isSet);
    std::optional<bool> findIsSet(const std::string& name) const;

    // Same reasoning as defineIsSet/findIsSet above, but for List<T> vs
    // Stack<T> (true = Stack, false = List) - needed only to disambiguate
    // `.push`/`.pop`, the two method names List<T> and Stack<T> share (see
    // docs/language/0035-stacks.md and IrGenerator::isStackExpr). A separate
    // map, not a reuse of isSetKinds_ above: a name is never simultaneously
    // a candidate for both disambiguations.
    void defineIsStack(const std::string& name, bool isStack);
    std::optional<bool> findIsStack(const std::string& name) const;

    // Same reasoning again, for LinkedList<T> vs Deque<T> (true = Deque,
    // false = LinkedList) - needed only to disambiguate push_front/
    // push_back/pop_front/pop_back, the method names LinkedList<T> and
    // Deque<T> share (see docs/language/0037-deques.md and
    // IrGenerator::isDequeExpr). A separate map again, for the same reason
    // isStackKinds_ isn't folded into isSetKinds_.
    void defineIsDeque(const std::string& name, bool isDeque);
    std::optional<bool> findIsDeque(const std::string& name) const;

    // Same reasoning again, for List<T>/Stack<T> vs PriorityQueue<T> (true =
    // PriorityQueue) - needed only to disambiguate push/pop/peek, the method
    // names PriorityQueue<T> shares with List<T>/Stack<T> (see
    // docs/language/0039-priority-queues.md and
    // IrGenerator::isPriorityQueueExpr). A separate map again, for the same
    // reason isStackKinds_ isn't folded into isSetKinds_.
    void defineIsPriorityQueue(const std::string& name, bool isPriorityQueue);
    std::optional<bool> findIsPriorityQueue(const std::string& name) const;

    // Same reasoning again, for Map<K,V>/Set<T> vs SortedMap<K,V> (true =
    // SortedMap) - needed only to disambiguate set/get/contains/remove, the
    // method names SortedMap<K,V> shares with Map<K,V>/Set<T> (see
    // docs/language/0040-sorted-maps.md and IrGenerator::isSortedMapExpr).
    // A separate map again, for the same reason isStackKinds_ isn't folded
    // into isSetKinds_.
    void defineIsSortedMap(const std::string& name, bool isSortedMap);
    std::optional<bool> findIsSortedMap(const std::string& name) const;

    // Same reasoning again, for Set<T>/Map<K,V>/SortedMap<K,V> vs
    // SortedSet<T> (true = SortedSet) - needed only to disambiguate
    // add/contains/remove, the method names SortedSet<T> shares with Set<T>
    // (add/contains/remove) and Map<K,V>/SortedMap<K,V> (contains/remove) -
    // see docs/language/0041-sorted-sets.md and
    // IrGenerator::isSortedSetExpr.
    void defineIsSortedSet(const std::string& name, bool isSortedSet);
    std::optional<bool> findIsSortedSet(const std::string& name) const;

    // Same reasoning again, for String vs Buffer (true = Buffer) - needed
    // only to disambiguate "append", the method name Buffer shares with
    // String (see docs/language/0043-buffer.md and
    // IrGenerator::isBufferExpr).
    void defineIsBuffer(const std::string& name, bool isBuffer);
    std::optional<bool> findIsBuffer(const std::string& name) const;

    // Same reasoning again, but string-valued rather than a bool disambiguating between two
    // fixed kinds - a match scrutinee (see docs/language/0064-enums.md and
    // IrGenerator::enumNameOfExpr) needs to know *which* of potentially many distinct
    // user-declared enum types a local name holds, not just a yes/no between two builtins.
    void defineEnumName(const std::string& name, std::string enumName);
    std::optional<std::string> findEnumName(const std::string& name) const;

    // Same reasoning again, but for *any* name whose own best-effort inferred Axea type name is
    // known (a literal's own primitive type, a struct literal's own type name, ...) - not tied
    // to any one disambiguation, unlike every map above. Used only by
    // IrGenerator::simpleTypeOfExpr, to resolve a plain local through one level of assignment
    // when deciding which alternative of a union type a value needs to be wrapped as (see
    // docs/language/0065-unions.md) - IrGenerator otherwise keeps no real type table at all
    // (see enumNameOfExpr's own comment), and this is deliberately no more general than that:
    // a best-effort forwarding of the same narrow signal, not a real type inferer.
    void defineSimpleType(const std::string& name, std::string simpleType);
    std::optional<std::string> findSimpleType(const std::string& name) const;

private:
    std::unordered_map<std::string, int> registers_;
    std::unordered_map<std::string, int> arrayLengths_;
    std::unordered_map<std::string, bool> isSetKinds_;
    std::unordered_map<std::string, bool> isStackKinds_;
    std::unordered_map<std::string, bool> isDequeKinds_;
    std::unordered_map<std::string, bool> isPriorityQueueKinds_;
    std::unordered_map<std::string, bool> isSortedMapKinds_;
    std::unordered_map<std::string, bool> isSortedSetKinds_;
    std::unordered_map<std::string, bool> isBufferKinds_;
    std::unordered_map<std::string, std::string> enumNames_;
    std::unordered_map<std::string, std::string> simpleTypes_;
    IrScope* parent_;
    bool isBarrier_;
};

class IrGenerator
{
public:
    IrProgram generate(const Program& program,
                       const std::unordered_map<std::string, std::vector<Capability>>& capabilities,
                       const std::unordered_map<std::string, std::vector<Region>>& regions);

private:
    // Where instructions currently being lowered get appended, the running
    // register counter for the enclosing function (shared across nested
    // instruction lists, e.g. an if's thenBlock/elseBlock), the enclosing
    // function (null at top level - top-level bindings were never subject to
    // capability/region analysis, so they never get Drop markers either),
    // and the struct-typed locals introduced directly in the *current* block
    // (rebound per block, so each block drops only its own).
    struct Context
    {
        std::vector<std::unique_ptr<IrInst>>* out;
        int* registerCount;
        const FunctionDecl* function;
        std::vector<int>* structLocals;
    };

    void registerStructs(const Program& program);
    IrFunction generateFunction(const FunctionDecl& function,
                                const std::vector<Capability>& capabilities,
                                const std::vector<Region>& regions);

    int lowerExpr(const Expr& expr, IrScope& scope, Context& ctx);
    // `match` (see docs/language/0064-enums.md) - lowers arms[armIndex..] into a chain of
    // nested IrBranch, mirroring exactly how `else if` chains already nest IfExpr (see
    // lowerExpr's own IfExpr case) - each non-last arm becomes `tag == <its own declared
    // variant index> ? <bind fields, lower body> : <recurse into the next arm>`; the last arm
    // (TypeChecker already guarantees full coverage - see MatchExpr's own checkExpr comment)
    // skips the comparison entirely and just lowers its own body directly, the same way a
    // final bare `else` block does. `tagReg` is computed once, by the caller, and threaded
    // through every recursion level rather than re-extracted at each one.
    int lowerMatchArm(int scrutineeReg,
                      int tagReg,
                      const EnumDecl& enumDecl,
                      const std::vector<MatchArm>& arms,
                      std::size_t armIndex,
                      IrScope& scope,
                      Context& ctx);
    void lowerStmt(const Stmt& stmt, IrScope& scope, Context& ctx);
    // Shared by `while` (condition non-null, dest discarded by the caller)
    // and `loop` (condition null, dest is the loop's produced value).
    // Returns the lowered IrLoop's own dest register.
    int lowerLoop(const Expr* condition, const Expr& body, IrScope& scope, Context& ctx);
    // Diffs `scope`'s current snapshot against the innermost enclosing
    // loop's pre-loop snapshot (top of loopPreSnapshotStack_) - used by
    // BreakStmt/ContinueStmt to record exactly which carried variables have
    // changed, and to what, by this specific point in the body. Empty if not
    // currently inside a loop (unreachable in a well-typed program, since
    // TypeChecker already rejects break/continue outside a loop).
    std::vector<std::pair<int, int>> currentLoopCarriedDiff(IrScope& scope) const;

    int emit(Context& ctx, std::unique_ptr<IrInst> inst);
    void emitVoid(Context& ctx, std::unique_ptr<IrInst> inst);
    int freshRegister(Context& ctx);

    // Deliberately approximate (see docs/language/0021-axea-ir.md): only
    // recognizes a direct struct literal, or a bare reference to a
    // struct-typed parameter, as worth a Drop marker - not the result of a
    // call or a field access.
    bool isObviouslyStructTyped(const Expr& expr, const FunctionDecl& function) const;

    // Best-effort resolution of a fixed array's compile-time-known element
    // count, used only to constant-fold `.length` (see docs/language/0031-arrays.md
    // and lowerExpr's FieldExpr case) - IrGenerator has no real type table by
    // design (every pass re-derives what it needs, e.g. isObviouslyStructTyped
    // above), so this recognizes just enough shapes for `.length` to be
    // zero-cost in the common cases: a direct array literal, an array-typed
    // function parameter, or a name already recorded in `scope`'s parallel
    // array-length map (populated by lowerStmt's AssignmentStmt case).
    // `function` is null at top level, mirroring Context::function.
    std::optional<int>
    arrayLengthOf(const Expr& expr, const FunctionDecl* function, const IrScope& scope) const;

    // Best-effort resolution of whether a Map/Set-typed expression is
    // specifically a Set (true) or a Map (false) - nullopt if it can't be
    // determined from the shapes recognized below. Needed only for
    // `.contains`/`.remove`, the two method names shared between
    // Map<i32,i32> and Set<i32> (every other method - List's push/pop,
    // Map's set/get, Set's add - is unambiguous by name alone; see
    // docs/language/0034-maps-and-sets.md). Mirrors arrayLengthOf's own
    // best-effort shape: recognizes a direct MapNewExpr/SetNewExpr, a
    // Map/Set-typed function parameter, a call to a function with a
    // Map/Set-typed return, or a name already recorded in scope's parallel
    // isSet map (populated by lowerStmt's AssignmentStmt case).
    std::optional<bool>
    isSetExpr(const Expr& expr, const FunctionDecl* function, const IrScope& scope) const;

    // Best-effort resolution of whether a List/Stack-typed expression is
    // specifically a Stack (true) or a List (false) - nullopt if it can't be
    // determined. Needed only for `.push`/`.pop`, the two method names
    // List<T> and Stack<T> share (`.peek` is unambiguous on its own - List
    // has no peek - so it never needs this). A sibling resolver, not a
    // generalization of isSetExpr itself, per this codebase's "each pass
    // re-derives independently" convention - same best-effort shape:
    // ListNewExpr/StackNewExpr literal, a List/Stack-typed function
    // parameter, a call to a function with that return type, or a name
    // already recorded in scope's parallel isStack map (populated by
    // lowerStmt's AssignmentStmt case). See docs/language/0035-stacks.md.
    std::optional<bool>
    isStackExpr(const Expr& expr, const FunctionDecl* function, const IrScope& scope) const;

    // Best-effort resolution of whether a LinkedList/Deque-typed expression
    // is specifically a Deque (true) or a LinkedList (false) - nullopt if it
    // can't be determined. Needed only for push_front/push_back/pop_front/
    // pop_back, the method names LinkedList<T> and Deque<T> share (see
    // docs/language/0037-deques.md). A sibling resolver again, mirroring
    // isStackExpr's exact shape: LinkedListNewExpr/DequeNewExpr literal, a
    // LinkedList/Deque-typed function parameter, a call to a function with
    // that return type, or a name already recorded in scope's parallel
    // isDeque map (populated by lowerStmt's AssignmentStmt case).
    std::optional<bool>
    isDequeExpr(const Expr& expr, const FunctionDecl* function, const IrScope& scope) const;

    // Best-effort resolution of whether a List/Stack/PriorityQueue-typed
    // expression is specifically a PriorityQueue (true) or not (false) -
    // nullopt if it can't be determined. Needed for push/pop/peek, the
    // three method names PriorityQueue<T> shares with List<T>/Stack<T> (see
    // docs/language/0039-priority-queues.md) - the first three-way
    // collision in this codebase. A sibling resolver again, not a
    // generalization of isStackExpr - checked *before* isStackExpr at every
    // call site that needs it, mirroring isStackExpr's exact shape: a
    // literal PriorityQueueNewExpr vs. StackNewExpr/ListNewExpr, a
    // PriorityQueue-typed function parameter (vs. Stack/List), a call to a
    // function with that return type, or a name already recorded in scope's
    // parallel isPriorityQueue map.
    std::optional<bool>
    isPriorityQueueExpr(const Expr& expr, const FunctionDecl* function, const IrScope& scope) const;

    // Best-effort resolution of whether a Map/Set/SortedMap-typed expression
    // is specifically a SortedMap (true) or not (false) - nullopt if it
    // can't be determined. Needed for set/get/contains/remove, the method
    // names SortedMap<K,V> shares with Map<K,V> (set/get/contains/remove)
    // and Set<T> (contains/remove) - see docs/language/0040-sorted-maps.md.
    // A sibling resolver again, mirroring isStackExpr/isPriorityQueueExpr's
    // exact shape: a literal SortedMapNewExpr vs. MapNewExpr/SetNewExpr, a
    // SortedMap-typed function parameter (vs. Map/Set), a call to a
    // function with that return type, or a name already recorded in
    // scope's parallel isSortedMap map.
    std::optional<bool>
    isSortedMapExpr(const Expr& expr, const FunctionDecl* function, const IrScope& scope) const;

    // Best-effort resolution of whether a Set/Map/SortedMap/SortedSet-typed
    // expression is specifically a SortedSet (true) or not (false) -
    // nullopt if it can't be determined. Needed for add/contains/remove,
    // the method names SortedSet<T> shares with Set<T> (add/contains/
    // remove) and Map<K,V>/SortedMap<K,V> (contains/remove) - see
    // docs/language/0041-sorted-sets.md. A sibling resolver again, mirroring
    // isSortedMapExpr's exact shape.
    std::optional<bool>
    isSortedSetExpr(const Expr& expr, const FunctionDecl* function, const IrScope& scope) const;

    // Best-effort resolution of whether a String/Buffer-typed expression is
    // specifically a Buffer (true) or a String (false) - nullopt if it
    // can't be determined. Needed only for "append", the one method name
    // Buffer shares with String (see docs/language/0043-buffer.md);
    // "append_line"/"clear"/"reserve"/"finish" are all unique names nothing
    // else uses, so they need no resolver. A sibling resolver again,
    // mirroring isStackExpr's exact shape.
    std::optional<bool>
    isBufferExpr(const Expr& expr, const FunctionDecl* function, const IrScope& scope) const;

    // Best-effort resolution of *which* registered enum type an expression's own value has -
    // nullopt if it can't be determined (see docs/language/0064-enums.md). Needed only for a
    // `match` scrutinee, which needs to know its own enum's declared variant *order* (to
    // compute each named arm's own numeric tag to compare against - see lowerMatchArm), the
    // one place this codebase's usual "no real type table" design genuinely needs a concrete
    // type *name*, not just a two-way yes/no the way isBufferExpr/isSetExpr/... above need.
    // Mirrors isBufferExpr's own exact resolution order: a direct variant-construction
    // expression, a function parameter's own declared type, a tracked local (scope.findEnumName),
    // or a called function's own declared return type.
    std::optional<std::string>
    enumNameOfExpr(const Expr& expr, const FunctionDecl* function, const IrScope& scope) const;

    // "T1 | T2 | ..." anonymous union types (see docs/language/0065-unions.md) - lower onto the
    // exact same enum-as-flattened-struct machinery a real `enum` already uses. Auto-registers
    // (if not already present) a synthetic EnumDecl for `canonicalName` into enums_ - one variant
    // per "|"-separated alternative, named after that alternative's own canonical type name, with
    // that one type as its single field - and returns it. A parallel copy of
    // TypeChecker::resolveUnionType's own registration logic (see that function's comment for why
    // this codebase's passes each keep their own copy rather than sharing one), minus the
    // validation TypeChecker already performed on the whole program before IrGenerator ever runs.
    const EnumDecl& registerUnionType(const std::string& canonicalName);
    // Best-effort resolution of an expression's own "simple" Axea type name (a primitive, or a
    // struct/enum's own name) - nullopt if it can't be determined. Used only to decide, at an
    // implicit-union-wrap boundary (assignment/return/call argument - see wrapForUnion), *which*
    // alternative of the target union a value corresponds to: a literal's own primitive type, a
    // struct literal's own type name, a parameter/tracked-local's own declared/inferred type
    // (IrScope::findSimpleType), or a called function's own declared return type. Deliberately no
    // more general than that (e.g. arithmetic/`if`/`match` results aren't resolved) - this
    // codebase's usual "no real type table" design (see enumNameOfExpr's own comment), scoped
    // down to exactly what union wrapping needs; a value TypeChecker has already proven is a
    // union member that this can't resolve is a real, if narrow, gap (see
    // docs/language/0065-unions.md's own Known Imprecision).
    std::optional<std::string>
    simpleTypeOfExpr(const Expr& expr, const FunctionDecl* function, const IrScope& scope) const;
    // If `declaredTypeName` is a union and `valueReg`'s own resolved simple type (via
    // simpleTypeOfExpr on `valueExpr`) is one of its alternatives, emits an IrStructNew wrapping
    // `valueReg` into that alternative's own tagged variant and returns the new register;
    // otherwise (not a union, or already exactly this union - e.g. forwarding an existing union
    // value through another union-typed boundary) returns `valueReg` unchanged. Shared by the
    // three boundaries TypeChecker's own isUnionMember gates (assignment, return, call argument).
    int wrapForUnion(int valueReg,
                     const Expr& valueExpr,
                     const std::string& declaredTypeName,
                     IrScope& scope,
                     Context& ctx);

    std::unordered_map<std::string, const StructDecl*> structs_;
    std::unordered_map<std::string, const EnumDecl*> enums_;
    // Owns every synthetic union EnumDecl registerUnionType builds (see that function's own
    // comment) - a real declared EnumDecl's pointer is instead owned by Program::items, which
    // already outlives the generator.
    std::vector<std::unique_ptr<EnumDecl>> unionDecls_;
    std::unordered_map<std::string, const FunctionDecl*> functions_;
    // Stack of pre-loop scope snapshots, one per currently-open loop (top =
    // innermost). Pushed/popped by lowerLoop; read by
    // currentLoopCarriedDiff for BreakStmt/ContinueStmt.
    std::vector<std::unordered_map<std::string, int>> loopPreSnapshotStack_;
};
