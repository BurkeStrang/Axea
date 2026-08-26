#pragma once

#include "ast/Stmt.hpp"
#include "ir/Ir.hpp"
#include "sema/RegionChecker.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    // `closureCapabilities`/`closureRegions` (see docs/language/0067-closures.md,
    // CapabilityChecker::closureEffectiveCapabilities, RegionChecker::closureRegions) - default
    // to empty so a caller that doesn't care about struct-typed closure parameters (most test
    // helpers) keeps compiling unchanged; a closure literal with no entry in either falls back to
    // generateClosureTrampoline's own original unconditional-IrMove behavior.
    IrProgram
    generate(const Program& program,
             const std::unordered_map<std::string, std::vector<Capability>>& capabilities,
             const std::unordered_map<std::string, std::vector<Region>>& regions,
             const std::unordered_map<const ClosureExpr*, std::vector<Capability>>&
                 closureCapabilities = {},
             const std::unordered_map<const ClosureExpr*, std::vector<Region>>& closureRegions = {},
             // Top-level bindings move-checking (RegionChecker::movedTopLevelBindings) determined
             // were consumed somewhere in the program's own top-level statements (e.g. `u =
             // User{...}; archive(u)`) - skipped by the synthetic top-level auto-print below (see
             // its own use), since printing a moved-away binding would read memory a real
             // user-written program already gave away. Defaults to empty so existing callers/test
             // helpers that never call RegionChecker at all keep compiling and behaving unchanged.
             const std::unordered_set<std::string>& movedTopLevelBindings = {});

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
        // Self-referential closures (see docs/language/0067-closures.md) - set only inside a
        // self-referential closure literal's own trampoline (by generateClosureTrampoline, once
        // capturesRegister is known), naturally inherited by every nested block's own copy of
        // `ctx` within that same trampoline (an if/loop body's own `Context blockCtx = ctx`), and
        // just as naturally *not* inherited into any other function/trampoline - each of those
        // always constructs its own fresh Context from scratch. lowerExpr's own CallExpr case
        // checks `selfRecursiveName` before the ordinary "closure-typed local" check: a call to
        // this exact name, from inside this exact trampoline, is a real recursive call to the
        // trampoline itself (an ordinary IrCall, with capturesRegister forwarded as the hidden
        // first argument) - not an indirect IrClosureCall through a closure value, since no such
        // value can exist yet at the point the closure literal's own body is still being
        // compiled.
        std::optional<std::string> selfRecursiveName;
        std::string selfRecursiveTrampolineName;
        int selfRecursiveCapturesRegister = -1;
        // Real memory reclamation (structs/enums only) - every currently-open scope's own list
        // of struct/enum-typed registers still needing a release, outermost (the function/
        // trampoline's own param frame) first. A pointer to one stack object owned by the
        // enclosing generateFunction/generateClosureTrampoline call, shared by pointer identity
        // across nested Context copies exactly like `registerCount` already is - a BlockExpr
        // pushes its own `structLocals` frame onto this same stack for the lexical extent of its
        // own body (ordinary C++ RAII-shaped push/pop; no early-return short-circuit exists at
        // this level, since a target-language `return` only appends an IrReturn to the
        // instruction *stream* - the C++ recursive lowering walk continues normally past it), so
        // at any point during lowering, walking every frame currently on this stack gives
        // exactly the set of struct/enum-typed bindings live at that exact source position. See
        // IrGenerator::emitReturn, the one consumer.
        std::vector<std::vector<int>*>* liveScopeStack = nullptr;
    };

    void registerStructs(const Program& program);
    IrFunction generateFunction(const FunctionDecl& function,
                                const std::vector<Capability>& capabilities,
                                const std::vector<Region>& regions);
    // Closures (see docs/language/0067-closures.md) - a closure literal's own body compiles to a
    // genuine new top-level function ("the trampoline"), reached only indirectly (through a
    // closure value's own field-0 function pointer - see IrClosureNew/IrClosureCall), never by
    // ordinary call syntax. `capturesStructName` is param 0's own type (always Borrowed/Read -
    // the closure *value* owns the captures struct, reused across every call of that same value,
    // never dropped by any one invocation); `capturedNames` are bound inside the body via
    // IrFieldGet off that param. Mirrors generateFunction's own shape closely, simplified by this
    // phase's own scope cut (a closure's own declared params are never struct-typed, so no
    // per-param region/capability array is threaded through the way generateFunction's own is).
    // `capabilities`/`regions` (see docs/language/0067-closures.md) mirror generateFunction's
    // own identical per-param arrays; null when `closureExpr` has no entry in either map passed
    // into generate() (no struct-typed param at all - the pre-existing, always-safe fallback of
    // an unconditional IrMove for every param).
    // `selfName` (see docs/language/0067-closures.md) - set only when this closure literal is
    // the direct RHS of an assignment to that name (self-referential/recursive closures);
    // nullopt otherwise. Stashed into the trampoline's own Context so lowerExpr's CallExpr case
    // can recognize a real recursive call back into this same trampoline.
    IrFunction generateClosureTrampoline(const ClosureExpr& closureExpr,
                                         const std::string& trampolineName,
                                         const std::string& capturesStructName,
                                         const std::vector<std::string>& capturedNames,
                                         const std::vector<Capability>* capabilities,
                                         const std::vector<Region>* regions,
                                         const std::optional<std::string>& selfName);
    // Implicit function-reference-to-closure coercion (see docs/language/0067-closures.md) - a
    // bare top-level function name, used where a closure value is expected, needs a trampoline of
    // its own too: unlike a real `fn(...){...}` literal, it captures nothing, but IrClosureCall's
    // ABI always calls through a captures-struct-taking function pointer, so a named function
    // (which takes no such hidden first param) can never be pointed to directly. This trampoline
    // just ignores its own (always-empty) captures param and forwards straight into `function`.
    IrFunction generateFunctionRefTrampoline(const FunctionDecl& function,
                                             const std::string& trampolineName,
                                             const std::string& capturesStructName);
    // nullopt if `valueExpr` isn't a bare top-level function name not otherwise shadowed by a
    // local (the ordinary case: the caller should lowerExpr `valueExpr` normally instead),
    // otherwise the register of a real closure value wrapping it, memoized per function name so
    // repeated references share one trampoline. Checked *before* lowerExpr at each of the three
    // boundaries this needs (assignment, return, call argument) - lowerExpr's own NameExpr case
    // would otherwise throw "undefined variable" resolving a bare function name (a function is
    // never bound in any IrScope; it's resolved by name, via functions_, only at an actual call
    // site) - mirrors Interpreter::tryWrapFunctionRef exactly, one IR-level register instead of
    // one runtime Value.
    std::optional<int> tryLowerFunctionRef(const Expr& valueExpr, IrScope& scope, Context& ctx);

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
    // Real memory reclamation (structs/enums only) - the one place an IrReturn is ever
    // constructed (replacing 4 previously-independent call sites: the explicit ReturnStmt case,
    // and the synthetic trailing return in generateFunction/generateClosureTrampoline/
    // generateFunctionRefTrampoline - duplicating this shape independently at each is exactly
    // how the early-return dead-code bug this fixes was introduced). `valueRegOrNegOne` is -1
    // for a bare/unit return. If it's also one of the registers about to be released below (a
    // self-aliasing return - `return x;` where `x` is itself a live struct/enum local/param),
    // retains it *first* so the matching release nets back to the original, still-positive
    // count instead of freeing a value still being handed to the caller - deliberately a
    // register-*presence* check against `*ctx.liveScopeStack`, not a "return type is
    // struct/enum" check: a value that isn't itself one of the tracked bindings (a fresh
    // literal, a value already forwarded from another call under this scheme's own "every
    // return hands back an already-correctly-refcounted value" convention) needs no retain at
    // all here - retaining it anyway would leak, since nothing would ever release the extra
    // count. Then walks every frame on `*ctx.liveScopeStack` (every struct/enum-typed binding
    // live at this exact point in the recursive lowering) and releases each, before finally
    // emitting the terminator itself. This is what makes a release actually reachable on an
    // early-return path, unlike the old structurally-appended-after-the-body drop loops it
    // replaces.
    void emitReturn(Context& ctx, int valueRegOrNegOne);
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
    // Real memory reclamation (structs/enums only) - true when `expr` is guaranteed to already
    // hold a fresh, uniquely-owned reference (refcount already exactly 1, from construction or
    // from a real Axea function's own `emitReturn`-based +1 return), needing no additional
    // retain wherever it's stored into a new binding/slot: a direct struct literal, an
    // `EnumName.Variant(...)`/`EnumName.Variant` construction, or an ordinary call to a *real,
    // user-defined* Axea function whose declared return type is struct/enum-typed. Deliberately
    // excludes every real impl *method* call and every builtin collection/string/buffer
    // operation (`.get()`/`.pop()`/`.peek()`/...): those don't go through generateFunction's own
    // emitReturn at all (a builtin op hands back a raw *aliased* pointer straight out of
    // existing storage, never a fresh one) - treating one as "fresh" would under-retain and risk
    // a real use-after-free once the original owner's own scope ends, so anything not provably
    // fresh conservatively needs a retain instead (a harmless, bounded extra retain+eventual-
    // leak in the worst case a genuinely-fresh value gets classified this way - never a
    // correctness bug, unlike the reverse mistake).
    bool isProvablyFreshStructValue(const Expr& expr) const;
    // Real memory reclamation (structs/enums only) - `simpleTypeOfExpr` plus the one shape it
    // doesn't recognize that this code specifically needs: `EnumName.Variant(args)`/
    // `EnumName.Variant` construction (a MethodCallExpr/FieldExpr, not a NameExpr/literal/plain
    // CallExpr - see isProvablyFreshStructValue's own identical check for this same shape). A
    // separate helper rather than extending simpleTypeOfExpr itself, which several other,
    // unrelated passes (union-wrap resolution) already depend on unchanged.
    std::optional<std::string> resolveStructOrEnumType(const Expr& expr,
                                                       const FunctionDecl* function,
                                                       const IrScope& scope) const;
    // Move semantics (structs/enums only) - shared by every site that stores a value into a
    // *new* struct/enum-owning slot (a struct literal's own field, an enum variant's own payload,
    // a union-wrap's own payload, a closure's own captures struct field, a take-call argument, a
    // collection insert): if `fieldAxeaType` is struct/enum-typed, removes `valueReg` from
    // whichever liveScopeStack frame currently tracks it for its own scope-exit drop (see
    // consumeTrackedRegister) - ownership has moved into this new slot, so the *old* owning scope
    // must no longer drop it (that would be a real double-free once a plain value is
    // structurally, unconditionally dropped - see emitStructDropHelpers). A harmless no-op if
    // valueReg was never tracked in the first place (a fresh literal/call result, or a Borrowed
    // value that was never added to any frame to begin with) - SSA register numbers are unique
    // within a function, so there's no risk of this touching an unrelated binding.
    void retainFieldValueIfNeeded(int valueReg,
                                  const Expr& sourceExpr,
                                  const std::string& fieldAxeaType,
                                  Context& ctx);
    // The actual mechanism above delegates to: searches every currently-active frame in
    // ctx.liveScopeStack (innermost to outermost) for `reg` and erases it from whichever one
    // holds it, so that frame's own scope-exit drop loop no longer touches it - the new owner
    // (wherever `reg` gets pushed into next, if anywhere) is what becomes responsible for its
    // eventual drop instead. Returns whether it was found/removed (unused by most callers, but
    // meaningful for emitReturn's own self-aliasing handling - see its own comment).
    bool consumeTrackedRegister(Context& ctx, int reg);

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
    // Shared<T> (the move-semantics work's own explicit, opt-in refcounting escape hatch) - same
    // lazy/idempotent registration shape as registerUnionType above, but registers a real 2-field
    // {i32 refcount, T value} StructDecl into structs_ instead of a synthetic EnumDecl into
    // enums_ - Shared<T> has no tag/variants, it's structurally just a struct. `elementTypeName`
    // is T's own bare struct/enum name (TypeChecker's ShareExpr case already guarantees T is
    // Struct or Enum, so this is always a clean identifier, no further mangling needed - unlike
    // a union's own '|'-separated member list). The mangled name is "Shared." + elementTypeName
    // (dot instead of the angle brackets/generics syntax, mirroring mangleUnionTypeName's own
    // "must be a legal LLVM identifier" reasoning).
    const StructDecl& registerSharedType(const std::string& elementTypeName);
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
    // Closures (see docs/language/0067-closures.md) - collects every bare NameExpr text
    // referenced anywhere in `expr`'s own subtree, unconditionally (own copy of the identical
    // over-approximating collector CapabilityChecker/Interpreter each already have - see
    // CapabilityChecker::collectReferencedNames's own comment for why this is safe).
    static void collectReferencedNames(const Expr& expr, std::unordered_set<std::string>& names);
    static void collectReferencedNames(const Stmt& stmt, std::unordered_set<std::string>& names);
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
    // Owns every synthetic Shared<T> StructDecl registerSharedType builds - same reasoning as
    // unionDecls_ above, just for structs_ instead of enums_.
    std::vector<std::unique_ptr<StructDecl>> sharedTypeDecls_;
    std::unordered_map<std::string, const FunctionDecl*> functions_;
    // Modules (see docs/language/0066-modules.md) - bare/real extern name -> the module that
    // declared it ("" for a root-file extern). An extern's own `name` is never module-qualified
    // (see ExternDecl::moduleName's own comment) - this map exists purely so lowerExpr's
    // MethodCallExpr case can tell a module-qualified *extern* call apart from a module-
    // qualified *function* call and lower the right callee text: the extern's own bare real
    // name (an IrCall must resolve to a real, externally-linked symbol like "sqrt", never a
    // synthetic "math.sqrt"), versus a function's own already-qualified name.
    std::unordered_map<std::string, std::string> externModules_;
    // Real module names, self-derived from functions_'s own already-'.'-qualified keys plus
    // externModules_'s own values (mirrors TypeChecker::registerSignatures's identical
    // derivation and its own comment on the harmless ImplDecl-mangled-name overlap). Consulted
    // by lowerExpr's MethodCallExpr case: `object` being a bare NameExpr matching this set means
    // "math.sqrt(...)" is a qualified module call, not a real method call.
    std::unordered_set<std::string> moduleNames_;
    // Closures (see docs/language/0067-closures.md) - every trampoline function synthesized by
    // lowerExpr's own ClosureExpr case, flushed into irProgram.functions at the end of generate()
    // (mirrors this phase's own union-type flatten-at-the-end timing: a closure's own trampoline
    // is discovered lazily, while lowering a function body, not up front the way registerStructs
    // discovers real top-level functions). unionCounter_-style monotonic counter for a unique
    // trampoline name ("closure$0", "closure$1", ...) and a unique captures-struct name
    // ("closure.captures.0", ...) per closure literal encountered.
    std::vector<IrFunction> closureTrampolines_;
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>>
        closureCaptureStructs_;
    int closureCounter_ = 0;
    // Implicit function-reference-to-closure coercion (see docs/language/0067-closures.md and
    // tryLowerFunctionRef) - one shared, always-empty captures struct type ("closure.captures
    // .fnref", lazily registered into closureCaptureStructs_ on first use) since a bare function
    // reference never captures anything; one trampoline per distinct function name (memoized here
    // rather than synthesized fresh per reference site, unlike a real closure literal's own
    // per-literal trampoline, since every reference to the same function needs an identical
    // trampoline body).
    std::string functionRefCapturesStruct_;
    std::unordered_map<std::string, std::string> functionRefTrampolines_;
    // Struct-typed closure parameters (see docs/language/0067-closures.md) - the two maps
    // generate()'s own new optional parameters are stashed into for the duration of one
    // generate() call, so lowerExpr's own ClosureExpr case (deep in the call tree, well past
    // generate()'s own top-level scope) can still look a given closure literal's own per-param
    // capability/region arrays up when it calls generateClosureTrampoline.
    std::unordered_map<const ClosureExpr*, std::vector<Capability>> closureParamCapabilities_;
    std::unordered_map<const ClosureExpr*, std::vector<Region>> closureParamRegions_;
    // Real memory reclamation (structs/enums only) - the same per-function `regions` map
    // generate()'s own top-level loop already receives as a parameter, stashed as a member so an
    // ordinary call site (lowerExpr's own CallExpr case, reached from inside a *different*
    // function's own body) can look up whether the *callee's* own param is Region::Owned - the
    // exact same criterion generateFunction's own release logic already uses - and retain a
    // struct/enum-typed argument before passing it, if so. Without this, a `take`-declared
    // struct/enum param gets released for real at the callee's own exit with no matching retain
    // at the call site, silently freeing memory a still-live caller-side binding (e.g. a
    // top-level variable, later auto-printed) may still depend on.
    std::unordered_map<std::string, std::vector<Region>> allFunctionRegions_;
    // Self-referential closures (see docs/language/0067-closures.md) - set by lowerStmt's own
    // AssignmentStmt case right before lowering a closure-literal RHS, consumed (read then
    // immediately cleared, via std::exchange) by lowerExpr's own ClosureExpr case a few frames
    // later - a narrow, deliberately ambient piece of state threaded this way rather than as a
    // new parameter on every lowerExpr/lowerStmt signature between the two, mirroring
    // TypeChecker::currentFunctionModule_'s identical "one well-scoped ambient field beats
    // threading one more parameter through everything" convention. Always nullopt again by the
    // time any *other* AssignmentStmt's own closure literal (nested inside this one's body, or
    // anywhere else) is reached, since the consuming read happens before any further recursive
    // lowering of this closure's own body.
    std::optional<std::string> pendingSelfReferenceName_;
    // Stack of pre-loop scope snapshots, one per currently-open loop (top =
    // innermost). Pushed/popped by lowerLoop; read by
    // currentLoopCarriedDiff for BreakStmt/ContinueStmt.
    std::vector<std::unordered_map<std::string, int>> loopPreSnapshotStack_;
};
