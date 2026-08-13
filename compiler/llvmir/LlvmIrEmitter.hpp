#pragma once

#include "ir/Ir.hpp"

#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// Emits textual LLVM IR (.ll) from Axea IR. Does not link against libLLVM -
// this is plain text generation, consistent with docs/language/0022-llvm-backend.md's
// framing: Axea's job stops at producing valid LLVM IR; the real LLVM
// toolchain (external, not linked into this binary) does everything after
// that (optimization, register allocation, codegen, object files). See that
// doc for the design rationale behind the choices below (heap-allocating
// every struct instance, phi-based branch merging, etc.).
class LlvmIrEmitter
{
public:
    std::string emit(const IrProgram& program);

private:
    // Per-function emission state, threaded through the recursive instruction
    // walk (mirrors IrGenerator::Context's role in the previous phase).
    struct FunctionContext
    {
        std::unordered_map<int, std::string> registerTypes; // Axea IR register -> LLVM type string

        // LLVM requires unnamed (numbered) SSA values within a function to be
        // defined in strictly increasing textual order - it is not enough
        // for every def to precede its uses; "%2 = ..." literally cannot
        // appear before "%1 = ..." in the output. Axea IR's own register
        // numbers don't satisfy this once GEP/malloc temporaries (which
        // don't exist in Axea IR at all) are interleaved, so LLVM register
        // numbers are allocated fresh, purely in emission order, and mapped
        // back to the Axea IR register they represent (see defineRegister).
        std::unordered_map<int, int> llvmRegisterOf; // Axea IR register -> LLVM SSA register number
        int nextLlvmRegister = 0;

        int nextLabel = 0;
        std::string currentLabel; // the basic block currently being appended to
        std::ostringstream* out = nullptr;

        // Per-currently-open-loop state (top = innermost), pushed/popped by
        // emitLoop; read by emitInstructions' IrBreak/IrContinue handling.
        // See docs/language/0028-loops.md for why loop-carried variables use
        // alloca/load/store (not phi) here.
        struct LoopEmitContext
        {
            std::string headerLabel;
            std::string exitLabel;
            // (pre-loop Axea register, body-end Axea register, alloca slot's LLVM register)
            std::vector<std::tuple<int, int, int>> carriedSlots;
            std::vector<std::pair<std::string, std::string>>
                breakValues; // (valueRefText, exitingLabel)
            bool anyBreakSeen =
                false; // including bare `break` - determines exit-block reachability
        };

        std::vector<LoopEmitContext> loopStack;
    };

    std::string llvmType(const std::string& axeaTypeName) const;
    std::string llvmReturnType(const std::optional<std::string>& returnType) const;
    std::string binOpMnemonic(TokenKind op) const;
    // Allocates a fresh, never-before-used LLVM register number - for
    // temporaries (GEP pointers, malloc size calcs) that have no
    // corresponding Axea IR register.
    int allocateRegister(FunctionContext& fctx) const;
    // Allocates a fresh LLVM register number and records it as the
    // definition site of the given Axea IR register. Must be called at the
    // point in emission order where that register's value is actually
    // produced (its "%N = ..." line), not ahead of time.
    int defineRegister(int axeaReg, FunctionContext& fctx) const;
    std::string ref(int axeaReg, const FunctionContext& fctx) const;
    std::pair<std::size_t, std::string> fieldIndexAndType(const std::string& structName,
                                                          const std::string& fieldName) const;
    std::string structNameFromPointerType(const std::string& pointerType) const;
    // "[N x T]*" -> "T" - mirrors structNameFromPointerType, used by
    // IrIndexGet's type inference and emitIndexGet/emitIndexSet's GEP
    // (see docs/language/0031-arrays.md).
    std::string arrayElementType(const std::string& pointerType) const;
    // "[N x T]*" -> N.
    int arraySizeFromPointerType(const std::string& pointerType) const;
    std::string typeOf(int reg, const FunctionContext& fctx) const;

    void inferTypes(const IrFunction& function, FunctionContext& fctx);
    void inferTypesInList(const std::vector<std::unique_ptr<IrInst>>& instructions,
                          FunctionContext& fctx);

    void collectStrings(const std::vector<std::unique_ptr<IrInst>>& instructions);
    // Registers a string constant (deduped by content) and returns its
    // global name - shared by real Axea string literals (collectStrings) and
    // the synthetic strings emitMain/emitStructPrintHelpers need (format
    // strings, punctuation, field/binding names).
    std::string hoistString(const std::string& text);
    // An inline `i8*` constant expression referencing a hoisted string - no
    // separate SSA register needed, since a GEP into a global constant is
    // itself a compile-time constant in LLVM IR. Only used by the
    // hand-emitted main/struct-print-helper text below, not by lowering.
    std::string stringPtrConstant(const std::string& text);
    void emitStringGlobals(std::ostringstream& out) const;
    void emitStructTypeDecls(std::ostringstream& out) const;

    // True if every path through this straight-line instruction list is
    // guaranteed to hit a Return, directly or via a Branch whose thenBlock
    // and elseBlock both alwaysTerminate. Mirrors
    // IrGenerator::alwaysTerminates (same shape, kept as a separate, pure
    // implementation here per this codebase's convention of each pass owning
    // its own walk). Used so emitFunction's defensive "should not happen"
    // trailing ret isn't appended after a Branch that's already fully
    // covered by explicit returns on both sides - which would double
    // terminate the merge block (already closed with `unreachable`).
    bool alwaysTerminates(const std::vector<std::unique_ptr<IrInst>>& instructions) const;
    // True if this instruction list reaches an IrBreak anywhere, recursing
    // into nested IrBranch but *not* into a nested IrLoop's own body (a
    // break there targets that inner loop, not this one). Used both by
    // alwaysTerminates (an infinite loop with no reachable break never
    // falls through) and emitLoop/emitInstructions (does the loop's exit
    // block have any predecessor at all).
    bool instructionsContainBreak(const std::vector<std::unique_ptr<IrInst>>& instructions) const;
    // First `break <value>`'s Axea register reachable in this instruction
    // list (same nested-loop boundary as instructionsContainBreak), or -1 if
    // none - used to type a LoopExpr's own dest register (every reachable
    // break within the same loop is already known, by TypeChecker's own
    // unification rule, to agree on type).
    int findFirstBreakValue(const std::vector<std::unique_ptr<IrInst>>& instructions) const;

    void emitFunction(const IrFunction& function, std::ostringstream& out);
    // One `void @axea.print.<TypeName>(%TypeName*)` helper per struct in
    // program.structs, pretty-printing "TypeName { field: value, ... }" to
    // match Interpreter's toString() exactly (recursing into nested
    // struct-typed fields). Named with an `axea.print.` prefix (not
    // `print_*`) so it can't collide with a user-defined Axea function -
    // Axea function names are emitted unmangled as `@name`.
    void emitStructPrintHelpers(const IrProgram& program, std::ostringstream& out);
    // `main`: lowers the top-level script (program.topLevel) exactly like a
    // zero-parameter function body, then prints each of
    // program.topLevelBindings as "name = value\n" - matching `ax run`'s own
    // printer (compiler/main.cpp) byte for byte, so the interpreter and the
    // compiled binary can be diffed directly.
    void emitMain(const IrProgram& program, std::ostringstream& out);
    void emitStructNew(const IrStructNew& structNew, FunctionContext& fctx);
    void emitFieldGet(const IrFieldGet& fieldGet, FunctionContext& fctx);
    void emitFieldSet(const IrFieldSet& fieldSet, FunctionContext& fctx);
    // Mirrors emitStructNew/emitFieldGet/emitFieldSet - same malloc + null-GEP
    // sizeof idiom, and same load/store-via-GEP shape, just addressed by a
    // register index instead of a named struct field (see
    // docs/language/0031-arrays.md).
    void emitArrayNew(const IrArrayNew& arrayNew, FunctionContext& fctx);
    void emitIndexGet(const IrIndexGet& indexGet, FunctionContext& fctx);
    void emitIndexSet(const IrIndexSet& indexSet, FunctionContext& fctx);
    void emitBranch(const IrBranch& branch, FunctionContext& fctx);
    // `while`/`loop`. See docs/language/0028-loops.md: loop-carried
    // variables become alloca/load/store (not phi), re-read at the top of
    // the header every iteration and re-read once more at the exit block for
    // code after the loop; the loop's own produced value (from `loop { ...
    // break x }`) becomes a phi at the exit block, generalizing
    // emitBranch's merge-phi to however many break sites exist.
    void emitLoop(const IrLoop& loop, FunctionContext& fctx);
    // Stores each (preLoopReg, currentReg) pair's currentReg into its slot
    // (looked up by preLoopReg in loopCtx.carriedSlots) - shared by the
    // natural loop-back edge (using IrLoop::carried) and every
    // IrBreak/IrContinue (using their own per-point carried snapshot).
    void storeCarriedValues(const std::vector<std::pair<int, int>>& carried,
                            const FunctionContext::LoopEmitContext& loopCtx,
                            FunctionContext& fctx);

    // Returns true if the list ended in a terminator (Return) - the caller
    // must not append a fallthrough branch/ret after that point.
    bool emitInstructions(const std::vector<std::unique_ptr<IrInst>>& instructions,
                          FunctionContext& fctx);

    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> structs_;
    std::unordered_map<std::string, std::optional<std::string>> functionReturnTypes_;
    std::vector<std::pair<std::string, std::string>>
        stringGlobals_; // (globalName, literalText), in discovery order
    std::unordered_map<std::string, std::string> stringGlobalByLiteral_;
    int nextGlobal_ = 0;
};
