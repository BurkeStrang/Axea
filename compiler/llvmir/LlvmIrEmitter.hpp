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
    // f64's own opcode table - fadd/fsub/fmul/fdiv for arithmetic, `fcmp`
    // with an *ordered* predicate (oeq/one/olt/ole/ogt/oge - false whenever
    // either operand is NaN, rather than trapping or being specially
    // rejected - see TypeChecker::isOrderableKind's own comment) for
    // comparison. A genuinely different opcode set from binOpMnemonic's
    // own integer one above, unlike i64 (which reuses every int opcode
    // unchanged, just at 64-bit width - LLVM's int opcodes are already
    // width-agnostic text, the same way icmp already was generic across
    // i32/i24/i64 before this).
    std::string floatBinOpMnemonic(TokenKind op) const;
    // Emits a real content comparison for one of `==`/`!=`/`</`<=`/`>`/`>=`
    // between two already-resolved bare i8* str pointers - reusing
    // registerKeyRuntime("str")'s own @axea.eq.str for equality (already
    // built for Map<K,V>/Set<T> key comparisons, just not previously wired
    // to the `==`/`!=` operators themselves) and registerOrderRuntime's own
    // @axea.less.str for ordering, combined via the standard "derive
    // <=/>/>= from a single strict less-than" identities (a<=b iff
    // not(b<a), a>b iff b<a, a>=b iff not(a<b)) since only a strict
    // less-than primitive exists. See docs/language/0042-string.md.
    // Takes the *Axea* dest register (not an already-allocated LLVM one) and
    // calls defineRegister on it internally, as the very last register
    // allocated in whichever branch runs - LLVM requires unnamed registers
    // in strictly increasing textual definition order, so any intermediate
    // register a branch needs (e.g. BangEqual's eqReg) must be allocated,
    // and its own line written, first.
    void emitStrComparison(int axeaDest,
                           TokenKind op,
                           const std::string& lhsPtr,
                           const std::string& rhsPtr,
                           FunctionContext& fctx);
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
    // Registers (if not already registered, memoized by canonical
    // "Optional<elem>" Axea string) a fresh named struct type
    // `%axea.Optional.<id> = type { i1, <payload> }` (see
    // docs/language/0052-optional.md) - a genuinely *named*, not anonymous,
    // by-value struct, unlike slice<T>'s own anonymous "{T*, i32}" fat
    // pointer: an anonymous "{i1, T}" would satisfy isSliceType's own loose
    // "starts with '{', doesn't end with '*'" test for several payload
    // types (e.g. Optional<i32> = "{i1, i32}"), silently corrupting any
    // code path that dispatches on that shape. No runtime functions to
    // register (unlike Map/Set/LinkedList/SortedMap) - Optional<T> needs
    // only the type declaration itself. Returns "%axea.Optional.<id>".
    std::string registerOptionalInstantiation(const std::string& payloadAxeaType);
    // Same registration, keyed directly by the payload's already-resolved
    // LLVM type text rather than an Axea type name - needed for Some(x),
    // whose payload type is read off `x`'s own already-inferred register
    // type (see inferTypesInList's IrOptionalNew case), not an Axea type
    // string. Both this and registerOptionalInstantiation above key off the
    // *same* underlying map (by LLVM text, registerOptionalInstantiation
    // just resolves the Axea name to LLVM text first) - so `Optional<i32>`
    // reached via `.parse<i32>()`, `Some(x: i32)`, or a declared
    // `Optional<i32>` return type all resolve to the exact same LLVM named
    // type, never three different ones for the same shape.
    std::string registerOptionalInstantiationForLlvmPayload(const std::string& payloadLlvmType);
    // "%axea.Optional." prefix only - unlike every collection's own isXType
    // check above, no further disambiguation is needed: nothing else in
    // this backend ever produces a "%axea.Optional."-prefixed type string.
    bool isOptionalType(const std::string& type) const;
    // True only for a genuine named-struct pointer ("%StructName*") - the
    // one shape structNameFromPointerType's own "strip leading '%'/
    // trailing '*'" logic is actually valid for. Every collection's own
    // outer header is instead either a *named* type used by value
    // (Optional<T>'s own "%axea.Optional.<id>", never reaching this check
    // - callers check isOptionalType first) or an *anonymous* struct
    // pointer ("{...}*" - List/Map/Set/etc., even the ones with a named
    // *inner* entry/node type, like Map<K,V>'s own
    // "{i32, i32, %axea.MapEntry.<id>**}*"), so this check is exactly
    // "does this actually start with '%', not '{'" - needed since a print-
    // dispatch site's "anything unrecognized must be a nested struct"
    // fallback would otherwise call structNameFromPointerType on a
    // collection type and silently corrupt (see
    // docs/language/0053-nested-generics.md).
    bool isNamedStructPointerType(const std::string& type) const;
    // The payload's own LLVM type - looked up by the full "%axea.Optional.<id>"
    // string (recorded at registration time; unlike sliceElementType/
    // listElementType above, not derived by substring-slicing the type text
    // itself, since a named type's own text carries no structural
    // information to slice out).
    std::string optionalPayloadType(const std::string& type) const;

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
    // resolveStrPtr's own logic, parameterized on an already-resolved LLVM
    // type + value ref rather than an Axea IR register - needed by emitJoin
    // (see docs/language/0050-collection-join-and-slicing.md), whose loop
    // body loads element values fresh via GEP each iteration, with no Axea
    // IR register (and therefore no typeOf/ref-able entry) of their own.
    // resolveStrPtr(reg, fctx) is now a thin wrapper over this.
    std::string resolveStrPtrOfType(const std::string& type,
                                    const std::string& valueRef,
                                    FunctionContext& fctx) const;
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
    // Shared by emitListPush/emitStackPush/emitPriorityQueuePush - the
    // List<T>/Stack<T>/PriorityQueue<T> analogue of ensureBufferCapacity
    // above, generalized over an arbitrary element type (Buffer's own
    // element type is always i8, so its helper has no elementType
    // parameter; this one does). Same doubling-or-exactly-`neededRef`
    // growth policy, same hand-verified alloca/load/store copy loop (no
    // phi), same "store new capacity/data back into the header in place,
    // then fall through to a shared done label" shape - just element-wise
    // (GEP-strided by elementType) rather than byte-wise. `neededRef` is
    // an element *count*, not a byte count (unlike ensureBufferCapacity's
    // own `neededRef`) - this function computes sizeof(elementType) itself
    // via the standard null-pointer GEP idiom. Capacity lives at field 2
    // of the header - *not* field 1, unlike Buffer's own {length,
    // capacity, data} order - deliberately, to keep the header's field-1
    // position permanently `T* data`, exactly as it always was before this
    // capacity field existed: every other List/Stack/PriorityQueue-reading
    // site in this file (printing, indexing, slicing, `.join()`) already
    // hardcodes "data is field 1" and needs zero changes as a result. It
    // also sidesteps a real, otherwise-silent type collision:
    // `{i32, i32, T*}*` (length, capacity, data - Buffer's own field
    // order) is byte-for-byte the same text `Deque<T>`'s own
    // `{i32, i32, T*}*` header (count, start, data) already produces,
    // which `isDequeType` distinguishes from every other collection by
    // that exact same prefix text - inserting capacity at field 1 would
    // make every `List<T>` indistinguishable from a `Deque<T>` of the same
    // element type.
    void ensureListCapacity(const std::string& headerType,
                            const std::string& objectType,
                            const std::string& listRef,
                            const std::string& elementType,
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
    // Lazily registers (memoized by target type, mirroring
    // registerMapInstantiation's own "register once" pattern) a single
    // shared `@axea.parse.i32`/`@axea.parse.i64`/`@axea.parse.f64`/
    // `@axea.parse.bool` runtime function for `.parse<T>()` (see
    // docs/language/0046-generic-methods.md), appended to
    // parseRuntimeText_. `i64` hand-rolls the identical digit loop `i32`
    // does, just at 64-bit width; `f64` calls libc's own `strtod` rather
    // than hand-rolling decimal-to-binary float parsing (a real, easy-to-
    // get-subtly-wrong algorithm - the same "reuse libc's well-tested
    // logic" reasoning registerI32ToStrRuntime's own choice of `sprintf`
    // over hand-rolled itoa already established). Returns the function's
    // own name.
    std::string registerParseRuntime(const std::string& targetType);
    void emitParse(const IrParse& parse, FunctionContext& fctx);
    void emitOptionalNew(const IrOptionalNew& optionalNew, FunctionContext& fctx);
    void emitOptionalIsSome(const IrOptionalIsSome& isSome, FunctionContext& fctx);
    void emitOptionalUnwrap(const IrOptionalUnwrap& unwrap, FunctionContext& fctx);
    // `.to_cstr()` (see docs/language/0048-ffi.md) - a representational
    // no-op (resolves `object` to a bare i8* via resolveStrPtr, then a
    // trivial self-bitcast), since cstr and str/String's own data pointer
    // are bit-identical; a real LLVM instruction is still emitted rather
    // than aliasing the source register directly, so `.to_cstr()`'s own
    // dest register is always independently defined like every other
    // instruction's dest here - `-O1` folds the redundant bitcast away.
    void emitToCstr(const IrToCstr& toCstr, FunctionContext& fctx);
    // Resolves `reg` to an i8* text representation, for any
    // text-representable type (see docs/language/Axea_Printing_Formatting.md
    // and TypeChecker::isTextRepresentable's identical rule): str/String
    // via the existing resolveStrPtr; i32/bool/char each via their own
    // dedicated conversion (registerI32ToStrRuntime/registerBoolToStrRuntime/
    // encodeCharUtf8). Shared by `print`/`write` (emitPrint) and
    // interpolation's own per-piece stringification
    // (emitBufferAppendValue) - the one operation both mechanisms
    // genuinely need in common.
    std::string stringifyValue(int reg, FunctionContext& fctx);
    // stringifyValue's own dispatch, parameterized on an already-resolved
    // LLVM type + value ref - same reasoning, and same "thin wrapper"
    // relationship, as resolveStrPtrOfType/resolveStrPtr above. emitJoin
    // is the one caller that actually needs this form.
    std::string stringifyValueOfType(const std::string& type,
                                     const std::string& valueRef,
                                     FunctionContext& fctx);
    // Lazily registers `@axea.i32.to_str(i32) -> i8*` (via a single
    // `sprintf` call - simpler and more robust than hand-rolling itoa,
    // matching this codebase's own "declare exactly the libc function
    // actually needed" convention established for @strlen) and
    // `@axea.bool.to_str(i1) -> i8*` (hand-rolled byte stores, mirroring
    // @axea.parse.bool's own self-contained style - no format-string
    // global needed for a fixed "true"/"false"). Each self-contained
    // within its own runtime-text block (own format-string global
    // included, for i32's case), so neither depends on the
    // collectStrings/emitStringGlobals hoisting pipeline's own timing.
    std::string registerI32ToStrRuntime();
    // `@axea.i64.to_str(i64) -> i8*` / `@axea.f64.to_str(double) -> i8*` -
    // same real-sprintf approach as registerI32ToStrRuntime above ("%lld"/
    // "%g" in place of "%d"; "%g", not "%f", to match Interpreter.cpp's
    // own toString exactly, so interpreted and compiled output stay
    // character-for-character identical).
    std::string registerI64ToStrRuntime();
    std::string registerF64ToStrRuntime();
    std::string registerBoolToStrRuntime();
    // `@axea.optional.<id>.to_str(%axea.Optional.<id>) -> i8*` (see
    // docs/language/0052-optional.md) - "Some(<payload>)" or "None",
    // reusing registerI32/I64/F64/BoolToStrRuntime for the payload half
    // exactly like stringifyValueOfType's own dispatch does. Memoized per
    // Optional instantiation (there's one distinct function per payload
    // type, same as every other instantiation-keyed registration here).
    // Payload restricted to i32/i64/f64/bool (throws otherwise) - the
    // exact set `.parse<T>()` itself produces; a str/char/struct payload
    // isn't supported for *printing* this phase (Some(x)/None themselves
    // still accept any payload type - only rendering one to text is
    // narrower).
    std::string registerOptionalToStrRuntime(const std::string& optionalType);
    // `@axea.strbuf.new() -> {i32,i32,i8*}*` / `@axea.strbuf.append(buf,
    // i8*) -> void` / `@axea.strbuf.finish(buf) -> i8*` (see
    // docs/language/0054-collection-printing.md) - a small growable-
    // string-buffer trio, self-contained (not sharing Buffer's own
    // inline-only codegen - see the header comment on strbufRegistered_
    // for why), every struct/collection stringifier below is built out of
    // calls into these. Also registers `@axea.char.to_str(i24) -> i8*`
    // (a standalone transcription of encodeCharUtf8's own UTF-8 encoding,
    // needed for the identical "can't call a live-fctx-only helper from
    // a standalone hand-written function" reason).
    void registerStrbufRuntime();
    // `@axea.tostring.<StructName>(ptr) -> i8*` for every struct in the
    // program, registered unconditionally and upfront (mirrors
    // emitStructPrintHelpers' own identical "build every struct's helper
    // regardless of whether it's actually used" choice) - unlike
    // collection stringifiers below, a struct's own fields are only
    // available via `program.structs`, not derivable from an LLVM type
    // string alone, so this can't be registered lazily from within
    // stringifyValueOfType the way collections are.
    void emitStructToStringHelpers(const IrProgram& program);
    // `@axea.tostring.<kind>.<id>(ptr) -> i8*` for a fixed array, List/
    // Stack/PriorityQueue (share List's own representation), Deque/Queue
    // (share Deque's), Map/Set/LinkedList/SortedMap/SortedSet
    // (count-only, matching the top-level binding printer's own
    // identical fallback for these) - dispatches on `llvmType`'s own
    // structural shape, memoized by that full LLVM type string (mirrors
    // registerOptionalInstantiationForLlvmPayload's own "no Axea-level
    // name available at this layer" reasoning).
    std::string registerCollectionToStrRuntime(const std::string& llvmType);
    // Shared by both emitStructToStringHelpers and
    // registerCollectionToStrRuntime - appends whatever LLVM text is
    // needed to stringify one value of `elementType` (referenced by
    // `valueRef`) into `body`, using `nextTmp` as a private fresh-name
    // counter scoped to the one standalone function currently being
    // built (these functions have no live FunctionContext/register
    // counter of their own - see registerStrbufRuntime's own comment),
    // and returns the resulting i8*'s own register name (or, for str -
    // already an i8* - `valueRef` itself, unchanged).
    std::string emitElementToStrCall(const std::string& elementType,
                                     const std::string& valueRef,
                                     std::ostringstream& body,
                                     int& nextTmp);
    // `print(...)`/`write(...)` (see
    // docs/language/Axea_Printing_Formatting.md) - stringifies each
    // argument and printf's it, space-separated, with an optional
    // trailing newline. Lazily registers three tiny format-string
    // globals (own runtime-text block, same self-contained reasoning as
    // registerI32ToStrRuntime above - referencing a global by name never
    // needs it declared earlier in the same module textually).
    void registerPrintRuntime();
    void emitPrint(const IrPrint& print, FunctionContext& fctx);
    // One piece of `"Hello {name}"`'s own desugaring (see
    // docs/language/Axea_Printing_Formatting.md) - stringifyValue's
    // result appended into `buffer`, otherwise structurally identical to
    // emitBufferAppend (same grow-check/copy-loop/null-terminate shape),
    // just starting from a stringified pointer instead of a
    // str-coercible one.
    void emitBufferAppendValue(const IrBufferAppendValue& appendValue, FunctionContext& fctx);
    // Lazily registers (once, mirroring registerParseRuntime's own
    // pattern at its smallest scale - a single fixed function, not one
    // per target type) `@axea.utf8.count(i8*) -> i32`, the shared
    // codepoint-counting routine `.length` now calls for str/String/
    // Buffer (see docs/language/0047-unicode.md). Returns the function's
    // own name ("@axea.utf8.count").
    std::string registerUtf8CountRuntime();
    // Lazily registers (once, same pattern as registerUtf8CountRuntime
    // above) `@axea.utf8.char_at(i8*, i32) -> i24`, the shared
    // codepoint-decoding routine single-character indexing (`s[i]`) calls
    // for str/String (see docs/language/0047-unicode.md). Walks whole
    // UTF-8 sequences (not one byte at a time, the way
    // registerUtf8CountRuntime's own counting loop can afford to), since
    // it has to decode - not just skip - the target codepoint once found.
    // Out-of-range (including a malformed/too-short string) returns
    // codepoint 0 - the walk never reads past the string's own nul
    // terminator, so this is memory-safe by construction with no separate
    // bounds check, mirroring registerUtf8CountRuntime's own identical
    // property. Returns the function's own name ("@axea.utf8.char_at").
    std::string registerUtf8CharAtRuntime();

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
    // Registers (if not already registered, memoized by canonical Axea
    // element/key type string) a strict less-than function for a given
    // orderable type, returning its name (e.g. "@axea.less.i32"). Only
    // ever called with a type TypeChecker::isOrderableKind accepts (i32,
    // char, str) - PriorityQueue<T>'s own push/pop sift comparisons and
    // SortedMap<K,V>/SortedSet<T>'s own generated AVL templates
    // (`<<LESSFN>>`) share this single primitive, mirroring how
    // registerKeyRuntime's hash/eq pair is shared by Map<K,V>/Set<T>. i32/
    // char each reduce to one `icmp slt` instruction; str is a hand-rolled
    // byte-walk lexicographic compare, matching registerKeyRuntime's own
    // @axea.eq.str byte-walk style (unsigned byte comparison, stopping at
    // the first difference or either string's nul terminator - the same
    // semantics a textbook strcmp has). See
    // docs/language/0039-priority-queues.md.
    std::string registerOrderRuntime(const std::string& axeaKeyType);
    // Emits "<destReg> = <lhsRef> <predicate> <rhsRef>" (predicate one of
    // "sle"/"slt") for one PriorityQueue<T> element comparison, returning
    // the freshly allocated destReg - str compares via
    // registerOrderRuntime's own @axea.less.str (not pointer identity),
    // routing "sle" through "not (rhs < lhs)" since only a strict
    // less-than primitive exists; every other orderable element type
    // (i32, char) still gets a plain icmp, exactly as before. Returns
    // (rather than taking) destReg for the same reason emitStrComparison
    // above takes an Axea register instead of a pre-allocated one: LLVM
    // requires unnamed registers in strictly increasing textual definition
    // order, so a caller-pre-allocated destReg could end up numerically
    // higher than an intermediate register ("sle"'s own notLessReg) this
    // function still needs to allocate and define first. Shared by all
    // three of PriorityQueue's own sift comparisons (push's sift-up, pop's
    // sift-down) - see docs/language/0039-priority-queues.md.
    int emitPriorityQueueCompare(const std::string& elementType,
                                 const std::string& predicate,
                                 const std::string& lhsRef,
                                 const std::string& rhsRef,
                                 FunctionContext& fctx);

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
    // `object[start..end]` etc (see docs/language/0045-str-slicing.md) -
    // resolves `object` to a bare i8* (resolveStrPtr, shared with
    // emitStringNew/emitStringAppend), defaults a missing `start` to 0 and
    // a missing `end` to a runtime @strlen call, then mallocs and copies
    // exactly `end - start` bytes plus a null terminator - a genuine copy,
    // not a zero-copy view, despite the design doc's own "no allocation"
    // framing (see that document's own Design section for why `str`'s
    // existing null-terminated-buffer representation makes a real
    // sub-range view impossible without changing `str` itself). Widened in
    // docs/language/0050-collection-join-and-slicing.md: when `object`
    // resolves to an Array or List<T> instead (isListType/leading '[' -
    // see resolveIndexableView), the same "malloc + copy exactly length
    // elements" shape builds a fresh List<T> instead, via a genuinely
    // separate branch (element-wise GEP copy, not a byte copy - "separate
    // over shared" for this whole operation, same as
    // emitBufferAppendValue's own precedent).
    void emitStrSlice(const IrStrSlice& strSlice, FunctionContext& fctx);

    // A read-only view over an Array or List<T>'s own backing storage,
    // shared by emitStrSlice's Array/List branch and emitJoin (see
    // docs/language/0050-collection-join-and-slicing.md) - the one
    // sub-computation both operations genuinely need in common: "get me a
    // flat T* data pointer and a length (i32 SSA ref, or a literal for a
    // fixed array), regardless of which of the two object shapes this is".
    // A small shared sub-computation, not a whole operation, mirroring
    // resolveStrPtr's own "shared helper" reasoning.
    struct IndexableView
    {
        std::string elementType; // LLVM element type, e.g. "i32", "i8*"
        std::string dataPtrRef;  // flat T* SSA value ref
        std::string lengthRef;   // i32 SSA value ref, or a literal integer text
    };

    IndexableView resolveIndexableView(int objectReg, FunctionContext& fctx);
    // Appends `textPtrRef` (an already-resolved i8* SSA value ref) to
    // `bufferRef` (an already-resolved {i32,i32,i8*}* SSA value ref) -
    // emitJoin's own primitive (see
    // docs/language/0050-collection-join-and-slicing.md), called twice per
    // loop iteration (once for the separator, once for the stringified
    // element). Structurally the same grow-check/copy-loop/null-terminate
    // shape emitBufferAppend/emitBufferAppendValue already use, just
    // parameterized on a raw ref instead of an Axea IR register - not
    // merged with either of those two (per this codebase's own "separate
    // over shared" convention for whole operations, already applied to
    // keep emitBufferAppend/emitBufferAppendValue themselves distinct).
    void appendTextToBuffer(const std::string& bufferRef,
                            const std::string& textPtrRef,
                            FunctionContext& fctx);
    // `object.join(separator)` (see
    // docs/language/0050-collection-join-and-slicing.md) - builds a fresh
    // Buffer inline (same initial state emitBufferNew produces), appends
    // element 0 unconditionally (skipped entirely if `object` is empty),
    // then loops from index 1 appending `separator` followed by each
    // further stringified element, before reading the buffer's own final
    // length/data fields directly into a fresh String header - the same
    // "steal the fields, no byte copy" finish emitBufferFinish already
    // does, just inlined here since this Buffer has no Axea-level
    // register/variable of its own to call IrBufferFinish against.
    void emitJoin(const IrJoin& join, FunctionContext& fctx);
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

    // Optional<T> monomorphization (see docs/language/0052-optional.md) -
    // same lazy-registration/dedup-by-content shape as
    // mapInstantiationIds_/mapSetTypeDeclsText_ above, at the smallest
    // scale here (a single type-decl line, no runtime functions). Keyed by
    // the payload's own LLVM type text (not an Axea type name string, the
    // way every other instantiation map above is) - see
    // registerOptionalInstantiationForLlvmPayload's own comment for why:
    // this is what lets Optional<i32> reached via `.parse<i32>()`,
    // Some(x: i32), or a declared `Optional<i32>` all collapse onto the
    // same LLVM named type. optionalPayloadTypeById_: assigned ID -> that
    // instantiation's payload, in LLVM type-string form (mirrors
    // mapValueLlvmTypeById_'s own by-ID lookup, needed at IrOptionalUnwrap
    // emission time).
    std::unordered_map<std::string, int> optionalInstantiationIds_;
    std::unordered_map<int, std::string> optionalPayloadTypeById_;
    int nextOptionalInstantiationId_ = 0;
    std::ostringstream optionalTypeDeclsText_;

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

    // Order (strict less-than) runtime (see registerOrderRuntime): canonical
    // Axea type string -> its lessFnName, memoized the same way
    // keyRuntimeFns_ above memoizes hash/eq, so a type reused across several
    // PriorityQueue<T>/SortedMap<K,V>/SortedSet<T> instantiations only gets
    // one less-than implementation.
    std::unordered_map<std::string, std::string> orderRuntimeFns_;

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

    // `.parse<T>()` (see docs/language/0046-generic-methods.md) - only two
    // possible target types this phase, so plain flags rather than a
    // dedup-by-string map/set are enough (mirrors the same "register
    // once" pattern as mapInstantiationIds_ above, at the smallest scale
    // that pattern comes in).
    bool parseI32Registered_ = false;
    bool parseI64Registered_ = false;
    bool parseF64Registered_ = false;
    bool parseBoolRegistered_ = false;
    std::ostringstream parseRuntimeText_;

    // `.length` on str/String/Buffer (see docs/language/0047-unicode.md) -
    // single shared runtime function, same "register once" pattern as
    // parseI32Registered_/parseBoolRegistered_ above, at an even smaller
    // scale (exactly one possible registration).
    bool utf8CountRegistered_ = false;
    std::ostringstream utf8CountRuntimeText_;

    // Single-character indexing (`s[i]`, see docs/language/0047-unicode.md)
    // - same "register once" pattern as utf8CountRegistered_ just above.
    bool utf8CharAtRegistered_ = false;
    std::ostringstream utf8CharAtRuntimeText_;

    // `print`/`write`/interpolation's own stringification (see
    // docs/language/Axea_Printing_Formatting.md) - each a single shared
    // runtime piece, same "register once" pattern as everything above.
    bool i32ToStrRegistered_ = false;
    bool i64ToStrRegistered_ = false;
    bool f64ToStrRegistered_ = false;
    bool boolToStrRegistered_ = false;
    bool optionalToStrGlobalsRegistered_ = false;
    std::unordered_map<std::string, std::string> optionalToStrFnByOptionalType_;
    // A small growable-string-buffer trio (see
    // docs/language/0054-collection-printing.md) - `@axea.strbuf.new`/
    // `.append`/`.finish`, self-contained standalone functions (not
    // reusing Buffer's own inline-only codegen, which assumes a live
    // FunctionContext tied to a *specific* already-known function's own
    // instruction stream - these need to be callable from any hand-
    // written stringify function instead). Every struct/collection
    // stringifier below is built out of calls into these three, so the
    // growable-buffer complexity is written exactly once.
    bool strbufRegistered_ = false;
    // Fixed, self-contained punctuation globals for
    // registerCollectionToStrRuntime (see
    // docs/language/0054-collection-printing.md) - *not* via
    // stringPtrConstant/hoistString: that shared mechanism is only safe
    // for callers registered before emitStringGlobals' own snapshot
    // point, but a collection stringifier can first be triggered lazily
    // from deep inside a function body's own codegen (`.join()`, `print`,
    // interpolation), well after that point - the exact "must appear
    // before use" class of timing bug docs/language/0052-optional.md's
    // own discovery-pass fix addresses for a different reason. Mirrors
    // registerOptionalToStrRuntime's own identical "declare fixed globals
    // directly in this function's own text" choice for "Some(%s)"/"None".
    bool collectionPunctuationGlobalsRegistered_ = false;
    // Struct/collection stringification (see
    // docs/language/0054-collection-printing.md) - `@axea.tostring.<Name>`
    // per struct shape (mirrors `@axea.print.<Name>`'s own per-shape
    // registration, memoized by struct name) and
    // `@axea.tostring.<kind>.<id>` per collection instantiation (memoized
    // by the collection's own full LLVM type text, since - unlike a
    // struct's name - that's the only stable per-shape key available at
    // this layer).
    std::unordered_map<std::string, std::string> structToStrFnByName_;
    std::unordered_map<std::string, std::string> collectionToStrFnByLlvmType_;
    bool printRuntimeRegistered_ = false;
    std::ostringstream toStrRuntimeText_;
    std::ostringstream printRuntimeText_;
};
