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

    // Not const: for Map<K,V>/Set<T> (see docs/language/0034-maps-and-sets.md's
    // generic rewrite), this is also the single point of monomorphized-
    // instantiation registration - the first time it resolves a given
    // canonical "Map<K,V>"/"Set<T>" string, it registers a fresh entry-type
    // ID (mirrors hoistString's own dedup-by-content pattern) and emits that
    // instantiation's named type + runtime functions as a side effect.
    std::string llvmType(const std::string& axeaTypeName);
    std::string llvmReturnType(const std::optional<std::string>& returnType);
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
                                                          const std::string& fieldName);
    std::string structNameFromPointerType(const std::string& pointerType) const;
    // "[N x T]*" -> "T" - mirrors structNameFromPointerType, used by
    // IrIndexGet's type inference and emitIndexGet/emitIndexSet's GEP
    // (see docs/language/0031-arrays.md).
    std::string arrayElementType(const std::string& pointerType) const;
    // "[N x T]*" -> N.
    int arraySizeFromPointerType(const std::string& pointerType) const;
    // A slice<T> is the anonymous LLVM struct "{T*, i32}" ("fat pointer" -
    // pointer + length, passed by value - see docs/language/0032-slices.md),
    // distinguished from every other type string here by starting with '{'.
    bool isSliceType(const std::string& type) const;
    // "{T*, i32}" -> "T".
    std::string sliceElementType(const std::string& type) const;
    // A List<T> is "{i32, T*}*" - a *pointer* to a small anonymous heap
    // record {length, data} (see docs/language/0033-lists.md), distinguished
    // from slice<T> (also '{'-prefixed, but never pointer-suffixed - a slice
    // is passed by value, never heap-allocated itself) by its trailing '*'.
    bool isListType(const std::string& type) const;
    // "{i32, T*}*" -> "T".
    std::string listElementType(const std::string& type) const;
    // Map<K,V>/Set<T> (see docs/language/0034-maps-and-sets.md's generic
    // rewrite) are now many distinct monomorphized instantiations, each with
    // its own numbered entry type (`%axea.MapEntry.<id>`/`%axea.SetEntry.<id>`)
    // - so unlike the old single-fixed-shape exact-string match, these check
    // the shared structural shape every instantiation's header has. Checked
    // *before* isListType at every call site that could see either: a
    // Map/Set header is also "{...}*"-shaped (3 fields, not 2), so it would
    // otherwise spuriously match isListType's own looser test.
    bool isMapType(const std::string& type) const;
    bool isSetType(const std::string& type) const;
    // "{i32, i32, %axea.MapEntry.<id>**}*" -> id (as text). Parsed straight
    // out of the type string itself - no separate reverse-lookup table
    // needed to go from "which register" to "which instantiation" once the
    // register's own LLVM type is already known.
    std::string mapSetInstantiationId(const std::string& type) const;
    // A Map instantiation's V doesn't appear anywhere in its own header type
    // string (unlike K, which every runtime function call site already gets
    // via typeOf on the key register) - `.get()`'s own dest register needs
    // it directly, so registerMapInstantiation records it per ID here.
    std::string mapValueLlvmType(const std::string& mapHeaderType) const;
    std::string typeOf(int reg, const FunctionContext& fctx) const;

    // Registers (if not already registered, memoized by the canonical
    // "Map<K,V>"/"Set<T>" Axea string) a fresh monomorphized instantiation:
    // assigns the next sequential ID, appends
    // `%axea.MapEntry.<id> = type { K, V, %axea.MapEntry.<id>* }` to
    // mapSetTypeDeclsText_, and appends that instantiation's own
    // `@axea.map.<id>.set/get/contains/remove/resize` functions to
    // mapSetRuntimeText_ (calling out to registerKeyRuntime for K's
    // hash/equality). Returns the full header type string
    // ("{i32, i32, %axea.MapEntry.<id>**}*"). See
    // docs/language/0034-maps-and-sets.md.
    std::string registerMapInstantiation(const std::string& keyAxeaType,
                                         const std::string& valueAxeaType);
    std::string registerSetInstantiation(const std::string& elementAxeaType);
    // Registers (if not already registered, memoized by canonical Axea key
    // type string) the hash/equality function pair for a given key type,
    // returning their names (e.g. ("@axea.hash.i32", "@axea.eq.i32")).
    // i32/bool/str get small fixed functions; struct keys get
    // `@axea.hash.<StructName>`/`@axea.eq.<StructName>` (name-based, not
    // numeric - generated once per distinct struct actually used as a key,
    // recursing into each field's own registerKeyRuntime call, combining
    // hashes and AND-ing equality); array/List keys get synthetic numeric
    // IDs, recursing into their own element type's registerKeyRuntime call
    // (arrays unroll - N is compile-time-known; List<T> needs a genuine
    // runtime loop, since its length isn't). See
    // docs/language/0034-maps-and-sets.md.
    std::pair<std::string, std::string> registerKeyRuntime(const std::string& axeaKeyType);

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
    void emitStructTypeDecls(std::ostringstream& out);

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
    // A fresh, empty {length: 0, data: null} heap record - same malloc +
    // null-GEP sizeof idiom as emitStructNew/emitArrayNew (see
    // docs/language/0033-lists.md).
    void emitListNew(const IrListNew& listNew, FunctionContext& fctx);
    // The hand-verified grow-on-every-push sequence (no amortized growth
    // this phase, a deliberate simplification - see docs/language/0033-lists.md):
    // GEP+load the current length, malloc a fresh buffer sized to length + 1,
    // a hand-rolled phi-based copy loop moving the old elements across (own
    // label numbering via fctx.nextLabel++, same convention emitBranch/
    // emitLoop already use), append the pushed value, then store the new
    // length/data back into the header's own fields in place.
    void emitListPush(const IrListPush& listPush, FunctionContext& fctx);
    // No bounds check (matches every other out-of-bounds case in this
    // backend - division, array/slice indexing: the interpreter checks,
    // compiled code does not); no shrink/realloc (matches the "no capacity
    // tracking" simplification - the buffer just stays at its previous size).
    void emitListPop(const IrListPop& listPop, FunctionContext& fctx);
    // Stack<T> (see docs/language/0035-stacks.md) - backed internally by
    // List<T>'s own machinery. emitStackNew/Push/Pop are structurally
    // identical to emitListNew/Push/Pop (separate functions, not shared,
    // per this codebase's "separate over shared" convention - they're
    // dispatched via distinct IR instruction types either way).
    void emitStackNew(const IrStackNew& stackNew, FunctionContext& fctx);
    void emitStackPush(const IrStackPush& stackPush, FunctionContext& fctx);
    void emitStackPop(const IrStackPop& stackPop, FunctionContext& fctx);
    // GEP+load the element at length-1, *without* the decrement-and-store-back
    // emitStackPop does - the one genuinely new operation this feature needs.
    void emitStackPeek(const IrStackPeek& stackPeek, FunctionContext& fctx);
    // A fresh {count: 0, bucketCount: 8, buckets: <8 nulls>} heap header (see
    // docs/language/0034-maps-and-sets.md) - same malloc + null-GEP sizeof
    // idiom as emitListNew, plus a second malloc for the initial bucket
    // array, zeroed via 8 unrolled stores (8 is a compile-time constant, so
    // this is cheaper than a real loop).
    void emitMapNew(const IrMapNew& mapNew, FunctionContext& fctx);
    void emitSetNew(const IrSetNew& setNew, FunctionContext& fctx);
    // Each of these is a single `call` into that instantiation's own shared
    // runtime functions (registerMapInstantiation/registerSetInstantiation
    // already emitted once, memoized per distinct (K,V)/(T) shape - see
    // docs/language/0034-maps-and-sets.md's generic rewrite for why Map/Set
    // don't inline their logic at each call site the way List's push/pop
    // do).
    void emitMapSet(const IrMapSet& mapSet, FunctionContext& fctx);
    void emitMapGet(const IrMapGet& mapGet, FunctionContext& fctx);
    void emitMapContains(const IrMapContains& mapContains, FunctionContext& fctx);
    void emitMapRemove(const IrMapRemove& mapRemove, FunctionContext& fctx);
    void emitSetAdd(const IrSetAdd& setAdd, FunctionContext& fctx);
    void emitSetContains(const IrSetContains& setContains, FunctionContext& fctx);
    void emitSetRemove(const IrSetRemove& setRemove, FunctionContext& fctx);
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
    // Callee name -> its declared parameter type strings, in order - used
    // only by IrCall emission to decide whether an argument needs an
    // array -> slice conversion (see docs/language/0032-slices.md).
    std::unordered_map<std::string, std::vector<std::string>> functionParamTypes_;
    std::vector<std::pair<std::string, std::string>>
        stringGlobals_; // (globalName, literalText), in discovery order
    std::unordered_map<std::string, std::string> stringGlobalByLiteral_;
    int nextGlobal_ = 0;

    // Map<K,V>/Set<T> monomorphization (see docs/language/0034-maps-and-sets.md's
    // generic rewrite). mapInstantiationIds_/setInstantiationIds_: canonical
    // "Map<K,V>"/"Set<T>" Axea string -> assigned sequential ID (mirrors
    // stringGlobalByLiteral_'s own dedup-by-content pattern above).
    // mapSetTypeDeclsText_/mapSetRuntimeText_ accumulate every registered
    // instantiation's named type + runtime functions, in registration order;
    // snapshotted into the final module text only once every function and
    // topLevel has been through inferTypes (which is what drives
    // registration - see llvmType), so registration order relative to
    // *writing* the final text doesn't matter.
    std::unordered_map<std::string, int> mapInstantiationIds_;
    // Map instantiation ID -> that instantiation's V, in LLVM type-string
    // form - see mapValueLlvmType.
    std::unordered_map<int, std::string> mapValueLlvmTypeById_;
    std::unordered_map<std::string, int> setInstantiationIds_;
    int nextMapInstantiationId_ = 0;
    int nextSetInstantiationId_ = 0;
    std::ostringstream mapSetTypeDeclsText_;
    std::ostringstream mapSetRuntimeText_;

    // Key hash/equality runtime (see registerKeyRuntime): canonical Axea key
    // type string -> its (hashFnName, eqFnName) pair, memoized so a key type
    // reused across several different Map/Set instantiations only gets one
    // hash/equality implementation, not one per instantiation. Array/List
    // keys get their own synthetic numeric IDs (a key *shape*, e.g. "[i32;4]"
    // used as a key, is a different thing from a whole Map/Set instantiation
    // - separate ID spaces).
    std::unordered_map<std::string, std::pair<std::string, std::string>> keyRuntimeFns_;
    int nextArrayKeyId_ = 0;
    int nextListKeyId_ = 0;
};
