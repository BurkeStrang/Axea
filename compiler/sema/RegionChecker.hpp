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
};

class RegionEnv
{
public:
    explicit RegionEnv(const RegionEnv* parent = nullptr);

    void define(const std::string& name, RegionInfo info);
    RegionInfo get(const std::string& name) const;

private:
    std::unordered_map<std::string, RegionInfo> bindings_;
    const RegionEnv* parent_;
};

class RegionChecker
{
public:
    void check(const Program& program,
               const std::unordered_map<std::string, std::vector<Capability>>& capabilities);

    const std::unordered_map<std::string, std::vector<Region>>& regions() const;

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
    void requireOwned(const RegionInfo& info, const FunctionDecl& function) const;

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
};
