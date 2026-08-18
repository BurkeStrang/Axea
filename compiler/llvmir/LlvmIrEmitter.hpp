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
    // LinkedList<T> (see docs/language/0036-linked-lists.md) - same
    // "structural-shape, not exact-string" reasoning as isMapType/isSetType
    // above, checked *before* isListType for the same reason (a LinkedList
    // header is also "{i32, ...}*"-shaped). Distinguished from Map/Set's own
    // "{i32, i32, ...}" shape by its second field being a node *pointer*
    // (%axea.LLNode.<id>*), not a plain i32 bucketCount.
    bool isLinkedListType(const std::string& type) const;
    // "{i32, %axea.LLNode.<id>*, %axea.LLNode.<id>*}*" -> id (as text).
    std::string linkedListInstantiationId(const std::string& type) const;
    // A LinkedList instantiation's element type doesn't appear anywhere in
    // its own header type string either (same problem as Map's V above,
    // mapValueLlvmType) - pop_front/pop_back's own dest register needs it
    // directly, so registerLinkedListInstantiation records it per ID here.
    std::string linkedListElementLlvmType(const std::string& linkedListHeaderType) const;
    // Deque<T> (see docs/language/0037-deques.md) - a pointer to a small
    // anonymous 3-field heap header {count, start, data}, no named type at
    // all (unlike Map/Set/LinkedList: its third field is a plain T*, not a
    // self-referential entry pointer). Checked *before* isListType for the
    // same "explicit, not accidental" reason every other 3-field collection
    // is; distinguished from Map/Set's own "{i32, i32, %axea.*Entry.<id>**}*"
    // shape by excluding the "%axea." prefix explicitly - checking for a
    // trailing "**}*" instead would misfire on Deque<str> (str is itself
    // "i8*", so Deque<str>'s own data field type is "i8**", the same
    // double-star suffix Map/Set's own entry-pointer-pointer field has).
    bool isDequeType(const std::string& type) const;
    // "{i32, i32, T*}*" -> "T".
    std::string dequeElementType(const std::string& type) const;
    // SortedMap<K,V> (see docs/language/0040-sorted-maps.md): a pointer to a
    // small anonymous 2-field heap header {count, root}, mirroring List<T>'s
    // own "always by pointer, mutated in place" header shape - but its
    // second field is a *named*, self-referential node pointer
    // (%axea.SortedMapNode.<id>*, declared by registerSortedMapInstantiation),
    // not a plain T* the way List<T>'s own data field is. Checked *before*
    // isListType for the same "explicit, not accidental" reason
    // isLinkedListType/isMapType/isSetType/isDequeType already are - a
    // SortedMap header is also "{i32, ...}*"-shaped, so it would otherwise
    // spuriously match isListType's own looser test.
    bool isSortedMapType(const std::string& type) const;
    // "{i32, %axea.SortedMapNode.<id>*}*" -> id (as text).
    std::string sortedMapInstantiationId(const std::string& type) const;
    // A SortedMap instantiation's V doesn't appear anywhere in its own
    // header type string (same problem as Map's own V, mapValueLlvmType) -
    // `.get()`'s own dest register needs it directly, so
    // registerSortedMapInstantiation records it per ID here.
    std::string sortedMapValueLlvmType(const std::string& sortedMapHeaderType) const;
    // SortedSet<T> (see docs/language/0041-sorted-sets.md) - same reasoning
    // as isSortedMapType, with %axea.SortedSetNode.<id> (key, height, left,
    // right - no value field) in place of %axea.SortedMapNode.<id>.
    // Checked *before* isListType for the same reason. No value-type
    // side-table needed (unlike SortedMap<K,V>'s own): a set has no V at
    // all.
    bool isSortedSetType(const std::string& type) const;
    // "{i32, %axea.SortedSetNode.<id>*}*" -> id (as text).
    std::string sortedSetInstantiationId(const std::string& type) const;
    // String (see docs/language/0042-string.md) - a single concrete type,
    // not generic, so unlike every collection above this needs neither a
    // registerXInstantiation call nor an id-parsing helper: llvmType
    // returns the same fixed "{i32, i8*}*" text every time, no
    // monomorphization at all. Deliberately the exact same LLVM type
    // List<i8> would have (i8 isn't nameable in Axea source, so this can
    // never actually collide with a real List<T> instantiation) - checked
    // *before* isListType only where that distinction actually matters
    // (the top-level print loop; `.length`'s field-get rides isListType's
    // own existing check for free, exactly like Stack<T>'s own header
    // reuse - see docs/language/0035-stacks.md).
    bool isStringType(const std::string& type) const;
    // Buffer (see docs/language/0043-buffer.md) - a single concrete type,
    // not generic, same "no monomorphization" reasoning as String above.
    // Unlike String, Buffer's own 3-field header ({i32 length, i32
    // capacity, i8* data}*) genuinely differs from String's 2-field one,
    // so `.length` does *not* get a free ride off any existing structural
    // check here - `isBufferType` is consulted directly in emitFieldGet
    // for both "length" and Buffer's own new "capacity" field, not just
    // for the top-level print loop the way isStringType is.
    bool isBufferType(const std::string& type) const;
    // A single Unicode scalar value (see docs/language/0044-char.md) -
    // deliberately given its own distinct integer width ("i24", not
    // "i32") specifically so this predicate - and the top-level print
    // dispatch that consults it - can always tell a char register apart
    // from a plain i32 one, something no textual "i32 vs i32" comparison
    // could ever do.
    bool isCharType(const std::string& type) const;
    // A Map instantiation's V doesn't appear anywhere in its own header type
    // string (unlike K, which every runtime function call site already gets
    // via typeOf on the key register) - `.get()`'s own dest register needs
    // it directly, so registerMapInstantiation records it per ID here.
    std::string mapValueLlvmType(const std::string& mapHeaderType) const;
    std::string typeOf(int reg, const FunctionContext& fctx) const;
    // Resolves `reg` to an i8* SSA value ref, extracting the data pointer
    // from a String header first if `reg` isn't already a bare str -
    // shared by emitStringNew/emitStringAppend, both of which can receive
    // either operand shape (see docs/language/0042-string.md and
    // TypeChecker::isStrCoercible's identical rule at the type-checking
    // layer). A small shared helper, not a duplicated snippet, mirroring
    // ref()/typeOf() themselves rather than this codebase's usual
    // "separate over shared" rule for whole *operations* - this resolves a
    // single value, not an operation.
    std::string resolveStrPtr(int reg, FunctionContext& fctx) const;
    // Shared by emitBufferAppend/emitBufferAppendLine/emitBufferReserve -
    // emits the "grow if `neededRef` (an i32 SSA value ref) exceeds the
    // buffer's own current capacity" branch: doubles capacity (or grows to
    // exactly `neededRef` if doubling still isn't enough), mallocs a fresh
    // data buffer of the new capacity, copies the buffer's own existing
    // `length` bytes across, then stores the new capacity/data back into
    // the header's own fields in place, before falling through to a
    // shared done label. A small shared sub-computation (not a whole
    // operation), mirroring resolveStrPtr's own "shared helper, not
    // duplicated snippet" reasoning just above.
    void ensureBufferCapacity(const std::string& bufferRef,
                              const std::string& neededRef,
                              FunctionContext& fctx);
    // Encodes a runtime i24 codepoint value into its own UTF-8 byte
    // sequence (1-4 bytes, plus a trailing null terminator) and returns an
    // i8* ref to the freshly malloc'd, always-5-byte buffer holding it -
    // used only by the top-level print dispatch's own char branch (see
    // docs/language/0044-char.md). A bounded 4-way branch on codepoint
    // range, not a loop - every char value already carries a valid
    // Unicode scalar value by construction (Parser::decodeCharLiteral is
    // the only way one is ever produced), so this never needs to validate
    // its input, only encode it.
    std::string encodeCharUtf8(const std::string& codepointRef, FunctionContext& fctx);

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
    // Registers (if not already registered, memoized by canonical
    // "LinkedList<elem>" Axea string) a fresh monomorphized node type +
    // push_front/push_back/pop_front/pop_back runtime functions - mirrors
    // registerMapInstantiation/registerSetInstantiation's own lazy-
    // registration pattern (see docs/language/0036-linked-lists.md). Returns
    // the full header type string ("{i32, %axea.LLNode.<id>*, %axea.LLNode.<id>*}*").
    std::string registerLinkedListInstantiation(const std::string& elementAxeaType);
    // Registers (if not already registered, memoized by canonical
    // "SortedMap<K,V>" Axea string) a fresh monomorphized node type +
    // height/rotateLeft/rotateRight/insertNode/minValueNode/removeNode/set/
    // get/contains/remove runtime functions - mirrors
    // registerMapInstantiation/registerLinkedListInstantiation's own lazy-
    // registration pattern (see docs/language/0040-sorted-maps.md). K needs
    // no hash/equality runtime the way Map<K,V>'s own key does (it's
    // restricted to i32, compared directly via icmp); V needs no runtime at
    // all (never compared, only stored, exactly like Map<K,V>'s own V).
    // Returns the full header type string
    // ("{i32, %axea.SortedMapNode.<id>*}*").
    std::string registerSortedMapInstantiation(const std::string& keyAxeaType,
                                               const std::string& valueAxeaType);
    // Registers (if not already registered, memoized by canonical
    // "SortedSet<T>" Axea string) a fresh monomorphized node type +
    // height/rotateLeft/rotateRight/insertNode/minValueNode/removeNode/add/
    // contains/remove runtime functions - mirrors
    // registerSortedMapInstantiation's own lazy-registration pattern (see
    // docs/language/0041-sorted-sets.md), with a 4-field node (key, height,
    // left, right - no value field) in place of SortedMap's own 5-field one.
    // Returns the full header type string
    // ("{i32, %axea.SortedSetNode.<id>*}*").
    std::string registerSortedSetInstantiation(const std::string& elementAxeaType);
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
    // A fresh {count: 0, root: null} heap header (see
    // docs/language/0040-sorted-maps.md) - same malloc + null-GEP sizeof
    // idiom as emitListNew/emitStackNew (2 fields, no bucket array unlike
    // Map/Set's own 3-field header - a tree needs no initial bucket
    // allocation). Direct C++ emission, not template text, mirroring
    // emitMapNew/emitSetNew's own choice.
    void emitSortedMapNew(const IrSortedMapNew& sortedMapNew, FunctionContext& fctx);
    // Each of these is a single `call` into that instantiation's own shared
    // runtime functions (registerSortedMapInstantiation already emitted
    // once, memoized per distinct (K,V) shape) - mirrors emitMapSet/
    // emitMapGet etc. exactly.
    void emitSortedMapSet(const IrSortedMapSet& sortedMapSet, FunctionContext& fctx);
    void emitSortedMapGet(const IrSortedMapGet& sortedMapGet, FunctionContext& fctx);
    void emitSortedMapContains(const IrSortedMapContains& sortedMapContains, FunctionContext& fctx);
    void emitSortedMapRemove(const IrSortedMapRemove& sortedMapRemove, FunctionContext& fctx);
    // A fresh {count: 0, root: null} heap header (see
    // docs/language/0041-sorted-sets.md) - mirrors emitSortedMapNew exactly,
    // just with the SortedSet node type in place of SortedMap's own.
    void emitSortedSetNew(const IrSortedSetNew& sortedSetNew, FunctionContext& fctx);
    void emitSortedSetAdd(const IrSortedSetAdd& sortedSetAdd, FunctionContext& fctx);
    void emitSortedSetContains(const IrSortedSetContains& sortedSetContains, FunctionContext& fctx);
    void emitSortedSetRemove(const IrSortedSetRemove& sortedSetRemove, FunctionContext& fctx);
    // String (see docs/language/0042-string.md) - the first collection
    // here whose "push"-equivalent needs a *runtime-computed* copy length
    // (via @strlen - str has no length field of its own, unlike every
    // element type every other collection copies). emitStringNew mallocs
    // header+buffer and copies text's own bytes plus a null terminator, in
    // one copy loop. emitStringAppend mallocs a new buffer, copies the old
    // content, then copies other's own bytes plus a fresh null terminator -
    // two loops, mirroring emitListPush's own "copy old, then append new"
    // shape generalized from a single scalar store to a whole byte range.
    // Both resolve their `text`/`other` operand's own inferred LLVM type
    // to decide whether it's already a bare i8* or needs the data pointer
    // extracted from a String header first (the "String lends a str"
    // coercion - see TypeChecker's own isStrCoercible).
    void emitStringNew(const IrStringNew& stringNew, FunctionContext& fctx);
    void emitStringAppend(const IrStringAppend& stringAppend, FunctionContext& fctx);
    // Buffer (see docs/language/0043-buffer.md) - the first collection
    // here with genuine *amortized* growth: emitBufferAppend/
    // emitBufferAppendLine only reallocate (doubling capacity, or exactly
    // enough if doubling still isn't sufficient) when the existing buffer
    // can't hold the new content - a real `br i1` branch over "grow" vs
    // "no grow needed", unlike every other push/append/set/add here, which
    // reallocates unconditionally every single call. Both paths converge
    // on a shared "now copy the new bytes in" tail that re-reads the
    // header's own (possibly just-updated) capacity/data fields fresh,
    // the same "reload from memory instead of phi" trick every multi-
    // predecessor merge in this backend already uses.
    void emitBufferNew(const IrBufferNew& bufferNew, FunctionContext& fctx);
    void emitBufferAppend(const IrBufferAppend& bufferAppend, FunctionContext& fctx);
    void emitBufferAppendLine(const IrBufferAppendLine& bufferAppendLine, FunctionContext& fctx);
    // `.clear()` - resets length to 0 and null-terminates at data[0]
    // without touching capacity/data at all - no loop, no branch, the
    // entire reason Buffer tracks capacity separately from length.
    void emitBufferClear(const IrBufferClear& bufferClear, FunctionContext& fctx);
    // `.reserve(n)` - shares its own "grow if not big enough" shape with
    // emitBufferAppend's own grow path, just targeting a caller-given
    // capacity instead of a to-be-appended length.
    void emitBufferReserve(const IrBufferReserve& bufferReserve, FunctionContext& fctx);
    // `.finish()` - a genuine ownership transfer: copies the buffer's own
    // length/data field *values* (not bytes) into a fresh String header,
    // then resets the buffer's own header to a fresh minimal allocation -
    // no byte copy at all, the cheapest possible correct implementation of
    // "hand this content over."
    void emitBufferFinish(const IrBufferFinish& bufferFinish, FunctionContext& fctx);
    // A fresh {length: 0, head: null, tail: null} heap header (see
    // docs/language/0036-linked-lists.md) - direct inline emission (not
    // template text, unlike push/pop below): straight-line, no branching, so
    // it needs none of the named-register machinery push/pop do. Same
    // malloc + null-GEP sizeof idiom as emitListNew/emitMapNew.
    void emitLinkedListNew(const IrLinkedListNew& linkedListNew, FunctionContext& fctx);
    // Each of these is a single `call` into that instantiation's own shared
    // runtime function (registerLinkedListInstantiation already emitted once,
    // memoized per distinct element type) - mirrors emitMapSet/emitMapGet
    // etc. exactly, for the same reason: maintaining the head/tail invariant
    // on an empty-list transition needs a real `br i1`, which named LLVM
    // registers (used in these template-text functions) support far more
    // easily than this backend's own strictly-numbered anonymous registers
    // would (see docs/language/0036-linked-lists.md).
    void emitLinkedListPushFront(const IrLinkedListPushFront& pushFront, FunctionContext& fctx);
    void emitLinkedListPushBack(const IrLinkedListPushBack& pushBack, FunctionContext& fctx);
    void emitLinkedListPopFront(const IrLinkedListPopFront& popFront, FunctionContext& fctx);
    void emitLinkedListPopBack(const IrLinkedListPopBack& popBack, FunctionContext& fctx);
    // A fresh {count: 0, start: 0, data: null} heap header (see
    // docs/language/0037-deques.md) - direct inline emission, same
    // malloc + null-GEP sizeof idiom as emitListNew (3 fields instead of 2).
    void emitDequeNew(const IrDequeNew& dequeNew, FunctionContext& fctx);
    // Shared copy-loop helper for emitDequePushFront/emitDequePushBack - see
    // their own doc comments below and the definition in the .cpp.
    int emitDequeCopyForPush(const std::string& elementType,
                             const std::string& oldDataRef,
                             int oldCountReg,
                             int oldStartReg,
                             int newDataReg,
                             int destOffset,
                             FunctionContext& fctx);
    // Direct inline C++ (not template text - unlike LinkedList's push, no
    // head/tail invariant to conditionally maintain, so no branching is
    // needed at all): mirrors emitListPush's malloc-a-buffer-of-size-
    // count+1 + hand-rolled copy loop exactly, except the copy source is
    // offset by the *old* start (reading data[start+i], not data[i]), and
    // push_front writes the new element at destination index 0 while
    // shifting the copied range to start at index 1 (push_back copies
    // straight across and appends at index count). New start is always
    // stored as 0 either way - see docs/language/0037-deques.md.
    void emitDequePushFront(const IrDequePushFront& pushFront, FunctionContext& fctx);
    void emitDequePushBack(const IrDequePushBack& pushBack, FunctionContext& fctx);
    // No loop, no branch - simpler than even emitListPop: pop_front reads
    // data[start] then increments start/decrements count; pop_back reads
    // data[start+count-1] then decrements count only (start untouched). No
    // bounds check and no reallocation, matching every other pop in this
    // backend.
    void emitDequePopFront(const IrDequePopFront& popFront, FunctionContext& fctx);
    void emitDequePopBack(const IrDequePopBack& popBack, FunctionContext& fctx);
    // Queue<T> (see docs/language/0038-queues.md) - backed internally by
    // Deque<T>'s own machinery, LLVM-identical to it (llvmType("Queue<T>")
    // produces the exact same text llvmType("Deque<T>") does), so no
    // isQueueType predicate exists anywhere: isDequeType's existing
    // structural check already matches a Queue<T> header by construction.
    // emitQueueNew/Enqueue/Dequeue are structurally identical to
    // emitDequeNew/emitDequePushBack/emitDequePopFront (separate functions,
    // not shared, per this codebase's "separate over shared" convention -
    // mirrors emitStackPush/Pop's own identical relationship to
    // emitListPush/Pop).
    void emitQueueNew(const IrQueueNew& queueNew, FunctionContext& fctx);
    void emitQueueEnqueue(const IrQueueEnqueue& enqueue, FunctionContext& fctx);
    void emitQueueDequeue(const IrQueueDequeue& dequeue, FunctionContext& fctx);
    // PriorityQueue<T> (see docs/language/0039-priority-queues.md) - a real
    // binary heap over a List<T>-identical header. emitPriorityQueueNew is a
    // direct copy of emitStackNew/emitListNew. emitPriorityQueuePush copies
    // emitStackPush's own malloc-and-copy-loop verbatim, then sifts the
    // newly appended element up toward the root. emitPriorityQueuePop moves
    // the last element into the vacated root slot, shrinks the length, then
    // sifts that element down. Both sift loops are hand-rolled
    // alloca/load/store-counter LLVM basic blocks (no phi), the first real
    // comparison-and-swap loops this backend has needed - every earlier
    // collection's push/pop was a renamed copy or a pure-arithmetic
    // shrink/grow, never a genuine reordering algorithm.
    void emitPriorityQueueNew(const IrPriorityQueueNew& priorityQueueNew, FunctionContext& fctx);
    void emitPriorityQueuePush(const IrPriorityQueuePush& priorityQueuePush, FunctionContext& fctx);
    void emitPriorityQueuePop(const IrPriorityQueuePop& priorityQueuePop, FunctionContext& fctx);
    // GEP+load at index 0 - the minimum always sits at the root by the heap
    // invariant, so (unlike emitStackPeek) this needs no arithmetic at all.
    void emitPriorityQueuePeek(const IrPriorityQueuePeek& priorityQueuePeek, FunctionContext& fctx);
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

    // LinkedList<T> monomorphization (see docs/language/0036-linked-lists.md)
    // - same lazy-registration-by-canonical-string pattern as
    // mapInstantiationIds_/mapSetTypeDeclsText_ above, kept in its own
    // buffers rather than shared with Map/Set's (separate collection,
    // separate node shape - "separate over shared").
    std::unordered_map<std::string, int> linkedListInstantiationIds_;
    // LinkedList instantiation ID -> that instantiation's element type, in
    // LLVM type-string form - see linkedListElementLlvmType.
    std::unordered_map<int, std::string> linkedListElementLlvmTypeById_;
    int nextLinkedListInstantiationId_ = 0;
    std::ostringstream linkedListTypeDeclsText_;
    std::ostringstream linkedListRuntimeText_;

    // SortedMap<K,V> monomorphization (see docs/language/0040-sorted-maps.md)
    // - same lazy-registration-by-canonical-string pattern as
    // linkedListInstantiationIds_/linkedListTypeDeclsText_ above, kept in
    // its own buffers ("separate over shared").
    std::unordered_map<std::string, int> sortedMapInstantiationIds_;
    // SortedMap instantiation ID -> that instantiation's V, in LLVM
    // type-string form - see sortedMapValueLlvmType.
    std::unordered_map<int, std::string> sortedMapValueLlvmTypeById_;
    int nextSortedMapInstantiationId_ = 0;
    std::ostringstream sortedMapTypeDeclsText_;
    std::ostringstream sortedMapRuntimeText_;

    // SortedSet<T> monomorphization (see docs/language/0041-sorted-sets.md)
    // - same lazy-registration-by-canonical-string pattern as
    // sortedMapInstantiationIds_/sortedMapTypeDeclsText_ above, kept in its
    // own buffers ("separate over shared"). No value-type side-table
    // needed (unlike SortedMap<K,V>'s own) - a set has no V at all.
    std::unordered_map<std::string, int> sortedSetInstantiationIds_;
    int nextSortedSetInstantiationId_ = 0;
    std::ostringstream sortedSetTypeDeclsText_;
    std::ostringstream sortedSetRuntimeText_;
};
