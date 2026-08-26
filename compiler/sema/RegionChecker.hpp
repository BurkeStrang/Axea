#pragma once

#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Runs after CapabilityChecker (docs/language/0020-compiler-architecture.md's
// "Escape Analysis" / "Region Analysis" stages). Checks that a borrowed
// (read/write) struct parameter never escapes its function via a return
// value, directly or nested inside a returned struct literal. See
// docs/language/0011-region-inference.md for the algorithm and its
// documented scope.
enum class Region
{
    Owned,
    Borrowed
};

constexpr std::string_view regionName(Region region)
{
    switch (region)
    {
        case Region::Owned: return "owned";
        case Region::Borrowed: return "borrowed";
    }
    return "unknown";
}

struct RegionInfo
{
    Region kind;
    std::string sourceParam; // populated when kind == Region::Borrowed, for error messages
    std::string structType;  // resolved struct type name; empty when not struct-typed
    // Populated only when this value is an array whose element type is
    // itself struct-typed (see docs/language/0031-arrays.md) - kept separate
    // from structType (which means "this value itself is a struct", used by
    // FieldExpr) so IndexExpr can promote it into the *result's* own
    // structType, letting `arr[i].field` chains resolve correctly.
    std::string elementStructType{};
    // Move semantics (structs/enums only - see docs/language/ move-checking RFC): true once an
    // Owned, struct/enum-typed binding has been consumed by some earlier use (assigned elsewhere,
    // passed to a take param, returned, captured, inserted into a collection, ...). A later read
    // of a name with moved == true is a compile error ("use of moved value"), not a runtime hazard.
    bool moved = false;
};

class RegionEnv
{
public:
    explicit RegionEnv(const RegionEnv* parent = nullptr);
    // Copyable: define()/markMoved() always write into *this* env's own bindings_, never through
    // parent_ - so a plain copy does NOT give branch isolation by itself (a move recorded in some
    // deeply-nested child scope created *inside* one branch never propagates back up into a
    // snapshot taken at the branch's own top level - only that child's own, later-discarded
    // bindings_ sees it). movedAccumulator_ (below) is what actually provides branch isolation.

    void define(const std::string& name, RegionInfo info);
    RegionInfo get(const std::string& name) const;
    // Non-throwing existence check, used by branch merges (see setMovedAccumulator) to skip a
    // name that was only ever declared *inside* one branch (and so has nothing to merge into the
    // outer scope - it simply doesn't exist there) rather than have get() throw "undefined
    // variable" for it.
    bool has(const std::string& name) const;
    // Marks an already-defined name (possibly inherited from an ancestor env) as moved, by
    // re-defining it in *this* env - which, for a name inherited from a parent, creates a
    // shadowing entry local to this env (and its own descendants). Also inserts into
    // *movedAccumulator_ if set (see below).
    void markMoved(const std::string& name);
    // Branch isolation for move-tracking (IfExpr/MatchExpr): every markMoved call, however deeply
    // nested inside the branch's own child scopes, needs to be visible to the *branch's own*
    // merge step afterward - but bindings_ mutations from a child scope die with that child scope
    // (see the copy note above). Fix: an accumulator set, inherited down through the parent chain
    // (RegionEnv's own constructor below propagates it to every descendant automatically), that
    // markMoved also writes into. A branch point creates one fresh accumulator per branch,
    // installs it via this setter on that branch's own top-level env, walks the branch (every
    // nested child scope inherits the same accumulator through its own constructor), then unions
    // the accumulated names and calls env.markMoved on each in the *outer*, shared env - which
    // correctly recurses into whatever accumulator (if any) that outer env itself belongs to, so
    // nested branches compose correctly.
    void setMovedAccumulator(std::unordered_set<std::string>* accumulator);

private:
    std::unordered_map<std::string, RegionInfo> bindings_;
    const RegionEnv* parent_;
    std::unordered_set<std::string>* movedAccumulator_ = nullptr;
};

class RegionChecker
{
public:
    // `closureCapabilities` (see docs/language/0067-closures.md and
    // CapabilityChecker::closureEffectiveCapabilities) - defaults to empty so every existing
    // caller that doesn't care about struct-typed closure parameter aliasing (most test helpers)
    // keeps compiling unchanged; a closure with no entry here is treated as if it declared no
    // struct-typed params at all (every param stays Region::Owned, this checker's own
    // pre-existing behavior before that gap was closed).
    void check(const Program& program,
               const std::unordered_map<std::string, std::vector<Capability>>& capabilities,
               const std::unordered_map<const ClosureExpr*, std::vector<Capability>>&
                   closureCapabilities = {});

    const std::unordered_map<std::string, std::vector<Region>>& regions() const;
    // Struct-typed closure parameters (see docs/language/0067-closures.md) - regions()'s own
    // closure-keyed analogue, the same relationship closureEffectiveCapabilities() has to
    // effectiveCapabilities().
    const std::unordered_map<const ClosureExpr*, std::vector<Region>>& closureRegions() const;
    // Top-level bindings (see docs/language/0020-compiler-architecture.md) that move-checking
    // determined were consumed somewhere in the program's own top-level statement sequence (e.g.
    // `u = User{...}; archive(u)` where `archive` takes `u`) - IrGenerator's own auto-generated
    // "print every top-level binding" pass must skip a name in this set, since it's synthesized
    // codegen, not real user-written code RegionChecker itself ever sees or could reject; printing
    // a moved-away binding would silently read memory that (once Part B of the move-semantics work
    // lands) has already been freed for real.
    const std::unordered_set<std::string>& movedTopLevelBindings() const;

private:
    void registerDecls(const Program& program);
    void checkFunction(const FunctionDecl& function, const std::vector<Capability>& capabilities);
    // currentLoopBreakRegions: null when not inside a loop; otherwise the
    // active innermost loop's collector of every reachable `break value`'s
    // region, used to determine a LoopExpr's own contributed region (mirrors
    // TypeChecker's currentLoopBreakTypes).
    RegionInfo regionOfExpr(const Expr& expr,
                            RegionEnv& env,
                            const FunctionDecl& function,
                            std::vector<RegionInfo>* currentLoopBreakRegions);
    void regionOfStmt(const Stmt& stmt,
                      RegionEnv& env,
                      const FunctionDecl& function,
                      std::vector<RegionInfo>* currentLoopBreakRegions);
    // `sourceExpr` is the expression `info` was computed from, needed for both diagnostics (naming
    // *which* value is unreturnable) and move-marking (only a NameExpr can be marked moved - a
    // temporary/literal has no binding to mark). Kept as the sole "safe to return" check (its
    // original job) with the added move-marking side effect for the Owned+NameExpr+struct/enum
    // case, so an already-returned local can't be silently reused afterward either.
    void requireOwned(const Expr& sourceExpr,
                      const RegionInfo& info,
                      RegionEnv& env,
                      const FunctionDecl& function) const;
    // The one general "consuming use" check (see the move-semantics RFC's core rule): a
    // struct/enum-typed source must be Owned and not already moved, or this throws; on success, if
    // sourceExpr is a plain name, marks it moved in env. Used at every consuming site *other* than
    // return (which keeps requireOwned's own distinct wording): struct-literal/enum-variant field
    // values, take-call arguments, FieldAssignStmt/IndexAssignStmt values, collection inserts.
    // A no-op for non-struct/enum values (info.structType.empty()) - this session's move-checking
    // is scoped to structs/enums only, matching the refcounting-removal work it replaces.
    void consumeOwned(const Expr& sourceExpr,
                      const RegionInfo& info,
                      RegionEnv& env,
                      const FunctionDecl& function) const;

    std::unordered_map<std::string, const FunctionDecl*> functions_;
    std::unordered_map<std::string, const StructDecl*> structs_;
    // `enum` declarations (see docs/language/0064-enums.md) - registered purely so
    // regionOfExpr's MethodCallExpr/FieldExpr cases can recognize `EnumName.Variant(args)`/
    // `EnumName.Variant` construction *before* recursing into `object` as an ordinary value
    // expression, which would otherwise throw "undefined variable" trying to look up a bare
    // enum type name in `env`.
    std::unordered_map<std::string, const EnumDecl*> enums_;
    // Modules (see docs/language/0066-modules.md) - registered for the identical reason enums_
    // is: regionOfExpr's MethodCallExpr case needs to recognize `math.sqrt(x)` *before*
    // recursing into `object` as an ordinary value, which would otherwise throw "undefined
    // variable: math" (this exact bug class - a bare type/module name mistaken for a bound
    // value in RegionChecker's own generic path - was first found, and documented, for a real
    // enum's own construction syntax; see docs/language/0064-enums.md). No extern *signatures*
    // are needed here (RegionChecker only cares about struct-typed aliasing, and an extern's
    // params/return are always FFI-safe primitives, never a struct - see
    // docs/language/0048-ffi.md), so this is just the set of real module names, derived from
    // both functions_'s own already-'.'-qualified keys and every registered ExternDecl's own
    // moduleName field.
    std::unordered_set<std::string> moduleNames_;
    std::unordered_map<std::string, std::vector<Region>> regions_;
    std::unordered_map<const ClosureExpr*, std::vector<Capability>> closureCapabilities_;
    std::unordered_map<const ClosureExpr*, std::vector<Region>> closureRegions_;
    // Every function's own effective (already-resolved) capabilities, as computed by
    // CapabilityChecker and handed to us via check()'s own `capabilities` parameter - kept as a
    // member (not just a local in check()) so a call-argument consuming-use check deep inside
    // regionOfExpr can look up whether the *callee's* corresponding param is `take`-declared,
    // without needing that callee's own regions_ entry to already exist yet (which, unlike
    // capabilities_, may not be true - regions_ is still being built function-by-function in the
    // same pass that consults it).
    std::unordered_map<std::string, std::vector<Capability>> capabilities_;
    std::unordered_set<std::string> movedTopLevelBindings_;
};
