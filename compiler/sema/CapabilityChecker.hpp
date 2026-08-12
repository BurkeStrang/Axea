#pragma once

#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"

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
    void checkMovesInStmt(const Stmt& stmt,
                          const FunctionDecl& function,
                          std::unordered_set<std::string>& moved) const;

    static std::optional<std::size_t> ownParamIndex(const std::string& name,
                                                    const FunctionDecl& function);
    static std::optional<std::size_t> rootParamIndex(const Expr& expr,
                                                     const FunctionDecl& function);

    std::unordered_map<std::string, const FunctionDecl*> functions_;
    std::unordered_map<std::string, std::vector<Capability>> inferred_;
    std::unordered_map<std::string, std::vector<Capability>> effective_;
};
