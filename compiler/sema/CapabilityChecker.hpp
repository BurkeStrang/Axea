#pragma once

#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Runs after TypeChecker (docs/language/0020-compiler-architecture.md's
// "Capability Analysis" stage). Infers the weakest read/write/take capability
// each function parameter needs, verifies any explicit declaration is
// sufficient, and checks that a `take`-consumed parameter is never used again
// afterward within the same block. See docs/language/0010-capability-inference.md
// and 0009-ownership.md for the algorithm and its documented limitations
// (no alias tracking, no cross-branch move merging).
class CapabilityChecker
{
public:
    void check(const Program& program);

    const std::unordered_map<std::string, std::vector<Capability>>& effectiveCapabilities() const;
    // Struct-typed closure parameters (see docs/language/0067-closures.md) - same data as
    // effectiveCapabilities() above, but keyed by each closure literal's own identity (a closure
    // has no name of its own to key a std::string map by, unlike a real FunctionDecl).
    const std::unordered_map<const ClosureExpr*, std::vector<Capability>>&
    closureEffectiveCapabilities() const;

private:
    void inferExpr(const Expr& expr, const FunctionDecl& function, bool& changed);
    void inferStmt(const Stmt& stmt, const FunctionDecl& function, bool& changed);
    void raise(const std::string& functionName,
               std::size_t paramIndex,
               Capability minimum,
               bool& changed);
    Capability effectiveOrInferred(const std::string& functionName, std::size_t paramIndex) const;

    void checkMovesInExpr(const Expr& expr,
                          const FunctionDecl& function,
                          std::unordered_set<std::string>& moved) const;
    // Closures (see docs/language/0067-closures.md) - collects every bare NameExpr text
    // referenced *anywhere* in `expr`'s own subtree, unconditionally (no scope-awareness - a
    // name shadowed by a nested block's own local is still collected). This over-approximation
    // is deliberate and safe for how it's actually used (deciding a closure's own move-only
    // capture set): the caller subtracts the closure's own top-level param names afterward,
    // which handles the common case (a closure param shadowing an outer name); a name shadowed
    // by a *nested* local *within* the closure body is a narrower, documented imprecision (see
    // that doc's own Known Imprecision) rather than one this collector tries to resolve
    // precisely - mirrors this whole checker's own already-documented "no alias tracking, no
    // cross-branch move merging" stance.
    static void collectReferencedNames(const Expr& expr, std::unordered_set<std::string>& names);
    static void collectReferencedNames(const Stmt& stmt, std::unordered_set<std::string>& names);

    void checkMovesInStmt(const Stmt& stmt,
                          const FunctionDecl& function,
                          std::unordered_set<std::string>& moved) const;

    static std::optional<std::size_t> ownParamIndex(const std::string& name,
                                                    const FunctionDecl& function);
    static std::optional<std::size_t> rootParamIndex(const Expr& expr,
                                                     const FunctionDecl& function);

    // Struct-typed closure parameters (see docs/language/0067-closures.md and that doc's own
    // guiding rule: "reuse the struct machinery until the representation genuinely can't be a
    // struct") - mints a synthetic FunctionDecl for `closureExpr` the first time it's seen
    // (memoized in closureFunctions_/closureOrder_ thereafter, keyed by the literal's own
    // identity since it has no name), registered into the *same* functions_/inferred_ maps a real
    // top-level function already uses - letting every other piece of this checker (raise,
    // effectiveOrInferred, the fixpoint loop's own per-function walk, the final declared-vs-
    // inferred merge, checkMovesInExpr's own driver loop) work completely unchanged, with zero
    // closure-specific logic of their own. `closureExpr.body` is walked directly by the caller
    // (inferExpr's own ClosureExpr case, exactly like `check()`'s own top-level loop walks a real
    // `function->body`) - the synthetic FunctionDecl's own `body` field is always null, since
    // nothing ever reads it.
    const FunctionDecl& registerClosure(const ClosureExpr& closureExpr);

    std::unordered_map<std::string, const FunctionDecl*> functions_;
    std::unordered_map<std::string, std::vector<Capability>> inferred_;
    std::unordered_map<std::string, std::vector<Capability>> effective_;
    std::unordered_map<const ClosureExpr*, std::unique_ptr<FunctionDecl>> closureFunctions_;
    std::unordered_map<const ClosureExpr*, std::vector<Capability>> closureEffective_;
    int closureCounter_ = 0;
};
