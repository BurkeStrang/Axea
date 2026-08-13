#pragma once

#include "lexer/TokenKind.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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

struct IrFunction
{
    std::string name;
    std::vector<std::string> paramNames;
    std::optional<std::string> returnType;
    std::vector<std::unique_ptr<IrInst>> body;
    int registerCount = 0;
};

struct IrProgram
{
    std::vector<IrFunction> functions;
    std::vector<std::unique_ptr<IrInst>> topLevel;
};
