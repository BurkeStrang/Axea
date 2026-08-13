#pragma once

#include "ast/Stmt.hpp"
#include "ir/Ir.hpp"
#include "sema/RegionChecker.hpp"

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

private:
    std::unordered_map<std::string, int> registers_;
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
    void lowerStmt(const Stmt& stmt, IrScope& scope, Context& ctx);

    int emit(Context& ctx, std::unique_ptr<IrInst> inst);
    void emitVoid(Context& ctx, std::unique_ptr<IrInst> inst);
    int freshRegister(Context& ctx);

    // Deliberately approximate (see docs/language/0021-axea-ir.md): only
    // recognizes a direct struct literal, or a bare reference to a
    // struct-typed parameter, as worth a Drop marker - not the result of a
    // call or a field access.
    bool isObviouslyStructTyped(const Expr& expr, const FunctionDecl& function) const;

    std::unordered_map<std::string, const StructDecl*> structs_;
};
