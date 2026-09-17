#pragma once

#include "lexer/TokenKind.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
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

struct IrConstInt64 final : IrInst
{
    std::int64_t value;
};

struct IrConstFloat final : IrInst
{
    double value;
};

struct IrConstBool final : IrInst
{
    bool value;
};

struct IrConstString final : IrInst
{
    std::string value;
};

// A single Unicode scalar value, already decoded to its own codepoint by
// the parser (see docs/language/0044-char.md) - a genuinely distinct
// instruction from IrConstInt (not reused) even though both just carry a
// 32-bit constant, because the LLVM backend gives char its own distinct
// integer width (`i24`, not `i32`) precisely so a char register can never
// be confused with a plain i32 one downstream (see LlvmIrEmitter::llvmType).
struct IrConstChar final : IrInst
{
    std::int32_t codepoint;
};

// `null` (see docs/language/0019-unsafe.md) - the untyped null pointer literal. Unlike every
// other IrConst* above, carries no value of its own kind's own type - `pointeeTypeName` is the
// canonical Axea-level pointee type text (e.g. "i32", "Node$i32") IrGenerator resolves from
// whatever *consumption* context this literal appeared in (a declared/field/param type, or the
// other operand's own type for `==`/`!=` - see IrGenerator.cpp's own `lowerNullExpr` and its call
// sites), since a bare `null` has no type of its own to derive it from the way every other
// IrConst* does.
struct IrConstNull final : IrInst
{
    std::string pointeeTypeName;
};

struct IrBinOp final : IrInst
{
    TokenKind op;
    int lhs;
    int rhs;
};

// `<operand> as <targetType>` (see docs/language/0005-type-system.md) - a
// numeric conversion between i32/i64/f64, resolved to the specific LLVM
// conversion opcode (sext/trunc/sitofp/fptosi) at the LlvmIrEmitter layer,
// based on the operand's own inferred type vs. targetType.
struct IrCast final : IrInst
{
    int operand;
    std::string targetType;
};

struct IrCall final : IrInst
{
    std::string callee;
    std::vector<int> args;
};

// `sizeof<TypeName>()` (see docs/language/0006-generics.md's own List<T> port follow-up) - no
// operand (there's no runtime value to evaluate, just a static type name), result is i64.
// `typeName` is always concrete by the time this is emitted - GenericMonomorphizer substitutes a
// bare type-parameter reference here exactly like IrCast::targetType.
struct IrSizeOf final : IrInst
{
    std::string typeName;
};

// `hash<T>(value)`/`keyEq<T>(a, b)` (see docs/language/0034-maps-and-sets.md's own "2026 Update") -
// `typeName` is the monomorphized concrete type, used to call `LlvmIrEmitter::registerKeyRuntime`'s
// own per-type hash/equality function pair, the same one Map<K,V>/Set<T>'s own `set`/`get`/etc used
// to call internally before they became real generic structs.
struct IrHashOf final : IrInst
{
    std::string typeName;
    int value;
};

struct IrKeyEq final : IrInst
{
    std::string typeName;
    int left;
    int right;
};

struct IrStructNew final : IrInst
{
    std::string typeName;
    std::vector<std::pair<std::string, int>> fields;
};

// `HeapArray<T>(n)` (see docs/language/0069-heap-array.md) - allocates a fresh wrapper struct
// (mangled `typeName`, e.g. "HeapArray.i32" - registerHeapArrayType's own key into structs_) plus
// its own separate `n`-element data buffer, `size` elements default-initialized to zero/null. Not
// lowered as an ordinary IrStructNew (whose fields are always already-computed values, not a
// freshly-malloc'd nested buffer).
struct IrHeapArrayNew final : IrInst
{
    std::string typeName;
    int size;
};

struct IrFieldGet final : IrInst
{
    int object;
    std::string field;
};

// A closure literal (see docs/language/0067-closures.md) - `capturesObject` is an already-built
// captures struct (an ordinary IrStructNew, reused unchanged - a captures struct's own fields are
// always plain Axea-typed values, never a function pointer, so it needs no dedicated instruction
// of its own). This instruction builds the closure *value* itself: the classic "fat pointer" pair
// {fn ptr, opaque captures ptr}, structurally keyed by signature alone (`paramTypes`/
// `returnType` - mirrors Optional<T>/Result<T,E>'s own "same shape regardless of instance"
// structural keying) rather than per-literal, since the captures are already hidden behind
// `capturesObject`'s own opaque pointer - LlvmIrEmitter registers (memoized) one
// `%axea.Closure.<id> = type { RetType (i8*, ParamTypes...)*, i8* }` per distinct signature, not
// per closure literal.
struct IrClosureNew final : IrInst
{
    std::string trampolineFunctionName;
    int capturesObject;
    std::vector<std::string> paramTypes;
    std::string returnType;
};

// Calling a closure *value* (see docs/language/0067-closures.md) - genuinely different from
// IrCall, which always targets a statically-known name: `closureObject` is a runtime value, and
// the call must go through its own field-0 function pointer, indirectly. `closureObject`'s own
// signature (needed to emit the exact `call RetType (ParamTypes...) %fnPtr(...)` text) is never
// duplicated here - LlvmIrEmitter re-derives it the same way it already re-derives every other
// register's own type, from where `closureObject` was itself constructed.
struct IrClosureCall final : IrInst
{
    int closureObject;
    std::vector<int> args;
};

struct IrFieldSet final : IrInst
{
    int object;
    std::string field;
    int value;
};

// `[e1, e2, ...]`. No element-type field: every element register's LLVM type
// is already inferred by the time this is reached (elements are always
// lowered, and therefore type-inferred, before the IrArrayNew instruction
// that references them), so LlvmIrEmitter derives the array's element type
// from elements.front() rather than needing it carried here - see
// docs/language/0031-arrays.md.
struct IrArrayNew final : IrInst
{
    std::vector<int> elements;
};

struct IrIndexGet final : IrInst
{
    int object;
    int index;
};

struct IrIndexSet final : IrInst
{
    int object;
    int index;
    int value;
};

// `*ptr` (read) inside `unsafe { }` (see docs/language/0019-unsafe.md) - a dedicated instruction
// rather than reuse of IrIndexGet's own generic dispatch-by-LLVM-type-text: IrIndexGet's own
// first check (str/cstr indexing, both rendering to plain "i8*") would otherwise misinterpret any
// raw pointer whose LLVM representation collides with str/cstr's own "i8*" as a UTF-8
// string-indexing operation instead of a raw scalar load.
struct IrDeref final : IrInst
{
    int pointer;
};

// `*ptr = value` inside `unsafe { }` - the write counterpart to IrDeref above, same reasoning.
struct IrDerefAssign final : IrInst
{
    int pointer;
    int value;
};

// `&name`'s own backing store (see docs/language/0019-unsafe.md) - a genuine, permanent stack
// slot for a local variable/parameter whose address is taken anywhere in its enclosing function
// or top-level script (see IrGenerator::collectAddressTakenNames), initialized immediately with
// `initialValue`. `dest` is the slot's own pointer register - every later read/write of the
// backed name reuses the existing IrDeref/IrDerefAssign instructions directly off this register
// (no dedicated load/store instruction needed - a stack slot is exactly the "real pointer"
// IrDeref's own source-agnostic codegen already handles, see LlvmIrEmitter::emitDeref). Unlike
// IrLoop::carried's own per-loop alloca/load/store text, this is function-scoped and emitted
// exactly once, at the name's first binding (a param, or an AssignmentStmt's first definition) -
// never inside a loop body's own re-executing block, so a name reassigned inside a loop it was
// already bound outside of reuses this same slot every iteration rather than allocating a fresh
// one each time.
struct IrAlloca final : IrInst
{
    int initialValue;
};

// `object[start..end]` / `object[..end]` / `object[start..]` / `object[..]`
// (see docs/language/0045-str-slicing.md). Originally str-coercible-only
// (`object` resolved to a bare i8* at the LLVM layer exactly like
// IrStringAppend's own operands are); widened in
// docs/language/0050-collection-join-and-slicing.md to also accept an
// Array or List<T> `object` (T restricted to i32/bool/char/str/String -
// see that document's own Design section for why struct-typed T is out of
// scope), producing a fresh List<T> instead of a fresh str in that case.
// Kept as one instruction rather than split in two, mirroring IrIndexGet's
// own precedent of one shared instruction dispatched by the object's
// inferred LLVM type at the LlvmIrEmitter layer - `object`'s own resolved
// type is all emitStrSlice/inferTypesInList need to pick the right
// lowering, exactly like emitIndexGet already does. `start`/`end` are each
// independently -1 when absent (mirrors IrInst's own dest == -1
// "no destination" convention) - -1 means "0" for start, "the object's
// own runtime length" for end (a real @strlen call for str, the object's
// own length field/static size for Array/List).
struct IrStrSlice final : IrInst
{
    int object;
    int start;
    int end;
};

// `object.join(separator)` (see
// docs/language/0050-collection-join-and-slicing.md) - `object` is an
// Array or List<T>, T restricted to i32/bool/char/str/String (the same
// isTextRepresentable set print()/interpolation already established);
// `separator` is str-coercible. Always produces a fresh, owned String,
// built the same way interpolation builds one (a Buffer under the hood,
// stringifying each element via the exact stringifyValue machinery
// print()/interpolation already share).
struct IrJoin final : IrInst
{
    int object;
    int separator;
};

// `List<T>`/`Stack<T>` are real, user-declared generic structs now, not compiler intrinsics (see
// docs/language/0006-generics.md's own List<T>/Stack<T> port follow-up and std/collections.ax) -
// there is no IrListNew/IrListPush/IrListPop/IrStackNew/IrStackPush/IrStackPop/IrStackPeek
// anymore; `.push`/`.pop`/`.get`/`.set`/`.peek` reach them via the general struct method dispatch
// (an ordinary IrCall), construction via their own `newList<T>()`/`newStack<T>()` generic
// top-level functions.

// `List<T>`/`Stack<T>`/`Deque<T>`/`Queue<T>`/`PriorityQueue<T>`/`LinkedList<T>` are all real,
// user-declared generic structs now, not compiler intrinsics (see docs/language/0006-generics.md's
// own port follow-up and std/collections.ax) - there is no IrDequeNew/IrDequePushFront/
// IrDequePushBack/IrDequePopFront/IrDequePopBack/IrQueueNew/IrQueueEnqueue/IrQueueDequeue/
// IrPriorityQueueNew/IrPriorityQueuePush/IrPriorityQueuePop/IrPriorityQueuePeek/IrLinkedListNew/
// IrLinkedListPushFront/IrLinkedListPushBack/IrLinkedListPopFront/IrLinkedListPopBack anymore;
// `.push_front`/`.push_back`/`.pop_front`/`.pop_back`/`.get`/`.set`/`.enqueue`/`.dequeue`/
// `.push`/`.pop`/`.peek` all reach their own real methods via the general struct method dispatch
// (an ordinary IrCall), construction via their own `newDeque<T>()`/`newQueue<T>()`/
// `newPriorityQueue<T>()`/`newLinkedList<T>()` generic top-level functions.

// SortedMap<K,V>/SortedSet<T> are both real, user-declared generic structs now (see
// docs/language/0040-sorted-maps.md's own "2026 Update" and docs/language/0041-sorted-sets.md's
// own "2026 Update") - there is no IrSortedMapNew/Set/Get/Contains/Remove or
// IrSortedSetNew/Add/Contains/Remove left here; `set`/`get`/`contains`/`remove`/`add` all reach
// the real struct's own methods via the general struct method dispatch (an ordinary IrCall),
// construction via their own `newSortedMap<K,V>()`/`newSortedSet<T>()` generic top-level
// functions - this is the last of these structs: every collection
// docs/language/0029-collections.md originally scoped as a compiler intrinsic is now real Axea
// source.

// `String(text)` - a fresh, owned copy of `text`'s own bytes, plus a null
// terminator (see docs/language/0042-string.md). `text` is a register (a
// str, or another String - LlvmIrEmitter resolves which at the point it
// reads `text`'s own inferred LLVM type), not a type name string - String
// isn't generic, unlike every collection's own *New instruction above.
struct IrStringNew final : IrInst
{
    int text;
};

// `string.append(other)` - no dest (unit); grows the buffer and copies
// `other`'s own bytes onto the end, mutating `string`'s own header fields
// in place (same "stable pointer, mutated in place" model every push/set/
// add here already uses).
struct IrStringAppend final : IrInst
{
    int string;
    int other;
};

// `Buffer()` - a fresh, empty buffer with a small initial allocation (see
// docs/language/0043-buffer.md) - no operands at all, unlike every
// collection's own *New instruction above: Buffer isn't generic (no type
// name to carry) and takes no constructor argument (unlike StringNewExpr's
// own `text`).
struct IrBufferNew final : IrInst
{
};

// `buffer.append(text)` - no dest (unit); the first collection here with
// genuine *amortized* growth - only reallocates (doubling capacity) when
// the existing buffer can't hold the new content, unlike every other
// push/append here, which reallocates unconditionally every call.
struct IrBufferAppend final : IrInst
{
    int buffer;
    int text;
};

// `buffer.append_line(text)` - same shape as IrBufferAppend, plus a
// trailing '\n'.
struct IrBufferAppendLine final : IrInst
{
    int buffer;
    int text;
};

// `buffer.clear()` - no dest (unit); resets length to 0 without releasing
// the allocated buffer, so a cleared Buffer can be refilled without
// reallocating - the entire point of tracking capacity separately from
// length.
struct IrBufferClear final : IrInst
{
    int buffer;
};

// `buffer.reserve(capacity)` - no dest (unit); grows the buffer's own
// allocation to at least `capacity` bytes without changing length or
// content, a no-op if already large enough.
struct IrBufferReserve final : IrInst
{
    int buffer;
    int capacity;
};

// `buffer.finish()` - dest is a String wrapping the buffer's own current
// content, with no byte copy at all (the buffer's own already-allocated,
// already-null-terminated data pointer is simply handed to the new String
// header directly) - a genuine ownership transfer, not a copy. `buffer`
// itself is left reset to a fresh, empty state afterward (see
// docs/language/0043-buffer.md), not left dangling - it remains safely
// reusable.
struct IrBufferFinish final : IrInst
{
    int buffer;
};

// `object.parse<T>()` (see docs/language/0046-generic-methods.md) - the
// first generic method call in this codebase. `object` is always
// str-coercible (str or String, resolved to a bare i8* at the LLVM layer
// exactly like IrStringAppend's own operands). `targetType` is the
// canonical Axea type string of the explicit type argument, restricted
// to "i32"/"bool" this phase (TypeChecker already rejects anything else
// before this instruction is ever lowered).
struct IrParse final : IrInst
{
    int object;
    std::string targetType;
};

// `Some(x)`/`None` (see docs/language/0052-optional.md) - constructs an
// Optional<T> value. `value` is -1 for None (no payload register to read).
// `payloadTypeName` is T's own canonical type name, needed at LLVM emission
// time to build the `{i1, T}` literal - unlike every other dest-typed
// instruction here, it can't be inferred from `value` alone, since None has
// no value register to infer from.
struct IrOptionalNew final : IrInst
{
    int value = -1;
    std::string payloadTypeName;
};

// `.is_some()`/`.is_none()` (see docs/language/0052-optional.md) - `negate`
// selects is_none (true) vs. is_some (false), avoiding a second near-
// identical instruction type for what's otherwise the same i1 read. Also
// shared verbatim by Result<T,E>'s own `.is_ok()`/`.is_err()` (see
// docs/language/0063-result.md): field 0 is "the positive-case tag" in
// both Optional's `{i1, T}` and Result's `{i1, T, E}` layout, so this
// instruction's own emission (a plain `extractvalue ..., 0`, optionally
// xor'd) never needed to know which of the two it's reading - only
// `LlvmIrEmitter`'s *type inference* for `.unwrap_or`/`?` (a different
// instruction, IrOptionalUnwrap below) ever has to branch on which
// concrete named type is involved.
struct IrOptionalIsSome final : IrInst
{
    int object;
    bool negate = false;
};

// `?`'s then-branch unwrap and `.unwrap_or`'s is-some branch both extract
// Optional<T>'s payload unconditionally (see docs/language/0052-optional.md)
// - the IrBranch each is always emitted inside already guarantees hasValue
// is true whenever this instruction actually runs. `field` defaults to 1
// (Optional's own, and Result<T,E>'s own Ok, payload position - both
// layouts agree there, see IrOptionalIsSome's own comment for why) and is
// set to 2 only by `?`'s Result-flavored Err-propagation path (see
// docs/language/0063-result.md), to extract the Err payload instead - the
// one position Optional's own `{i1, T}` layout has no equivalent of.
struct IrOptionalUnwrap final : IrInst
{
    int object;
    int field = 1;
};

// `Ok(x)`/`Err(e)` (see docs/language/0063-result.md) - constructs a
// Result<T,E> value, mirroring IrOptionalNew's own shape but for a
// 3-field `{i1, T, E}` literal instead of `{i1, T}`: `value` is always a
// real register (unlike IrOptionalNew's `value == -1` None case - Ok/Err
// always carry a real payload, just never *both* at once).
// `otherPayloadTypeName` is the canonical type name of whichever slot
// `value` does *not* cover (E when isOk, T when !isOk) - always needed,
// since a single register can only ever tell LLvmIrEmitter about one of
// the two type parameters, unlike IrOptionalNew's `payloadTypeName`,
// which is only ever needed for the *no-value-at-all* None case.
struct IrResultNew final : IrInst
{
    bool isOk;
    int value;
    std::string otherPayloadTypeName;
};

// `object.to_cstr()` (see docs/language/0048-ffi.md) - `object` is always
// str-coercible (str or String), resolved to a bare i8* exactly like
// IrParse's own operand. A representational no-op (cstr and str/String's
// own underlying data pointer are bit-identical - see
// docs/language/0042-string.md), so this exists purely to carry the type
// distinction through to the dest register.
struct IrToCstr final : IrInst
{
    int object;
};

// `print(...)`/`write(...)` (see
// docs/language/Axea_Printing_Formatting.md) - compiler builtins, not
// ordinary calls (no `functions_`/`externs_` lookup at all). `args` may
// be empty (`print()` alone just prints a newline). `addNewline`
// distinguishes the two - the only difference between them.
struct IrPrint final : IrInst
{
    std::vector<int> args;
    bool addNewline;
};

// One piece of `"Hello {name}"`'s own desugaring (see
// docs/language/Axea_Printing_Formatting.md) - stringifies `value`
// (restricted by TypeChecker to i32/bool/char/str/String) and appends
// the result to `buffer`, mirroring IrBufferAppend's own shape but for a
// value that isn't already str-coercible on its own.
struct IrBufferAppendValue final : IrInst
{
    int buffer;
    int value;
    // Raw `{expr:spec}` format-spec text (see
    // docs/language/0055-numeric-format-specs.md), e.g. "05"/".2"/"08b" -
    // empty for a plain `{expr}` piece with no format spec, which appends
    // `value`'s own ordinary stringifyValue() representation exactly as
    // before this phase. Always empty when `debug` below is set.
    std::string formatSpec;
    // `{expr:?}` (see docs/language/0058-debug-formatting.md) - debug
    // representation: identical to the unformatted case except for
    // str/String, which get wrapped in quotes. `{expr=}`'s own self-doc
    // prefix needs no field here at all - IrGenerator lowers it as a
    // separate, ordinary IrBufferAppend of a literal "<text>=" string,
    // emitted immediately before this instruction.
    bool debug = false;
};

// `Map<K,V>`/`Set<T>` are real, user-declared generic structs now, not compiler intrinsics (see
// docs/language/0034-maps-and-sets.md's own "2026 Update") - there is no IrMapNew/IrMapSet/
// IrMapGet/IrMapContains/IrMapRemove/IrSetNew/IrSetAdd/IrSetContains/IrSetRemove anymore;
// `.set`/`.get`/`.contains`/`.remove`/`.add` all reach their own real methods via the general
// struct method dispatch (an ordinary IrCall), construction via their own `newMap<K,V>()`/
// `newSet<T>()` generic top-level functions. `hash<T>()`/`keyEq<T>()` (`IrHashOf`/`IrKeyEq`
// above) are the new generic-code-facing entry point into the same `registerKeyRuntime` hash/
// equality generation these methods' own bodies now call directly instead of a dedicated
// instruction.

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
    // (thenReg, elseReg, destReg) per outer-scope name either branch reassigned to a different
    // register than it had before the branch - thenReg/elseReg are whichever register that
    // branch's own IrScope rebound the name to (or the pre-branch register, if that particular
    // branch never touched it); destReg is a freshly allocated register code after the branch
    // reads instead of the original pre-branch one. Mirrors thenValue/elseValue/dest above
    // exactly, just generalized to every other name either branch's own statements reassigned,
    // not only the branch-expression's own result. Populated by
    // IrGenerator::mergeBranchScopes, reconciled into a real phi by LlvmIrEmitter::emitBranch
    // (see docs/language/0021-axea-ir.md's own follow-up correcting its own originally-
    // documented "no merge" limitation, accurate when this IR was only ever printed for `ax ir`,
    // wrong now that it drives real LLVM codegen).
    std::vector<std::tuple<int, int, int>> carriedMerges;
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

// A struct/enum-typed binding (local or param) at the end of its own lifetime - move semantics
// (structs/enums only): single ownership is proven at compile time (see RegionChecker's
// move-checking), so LlvmIrEmitter lowers this to a genuine, unconditional
// `call void @axea.drop.<Name>(...)`, recursing into owned fields and freeing (see
// emitStructRefcountHelpers) - no runtime refcount involved. IrGenerator only ever emits this for
// a register that's still tracked in its owning scope's frame at the point of drop -
// IrGenerator::consumeTrackedRegister is what removes a register from its frame the moment
// ownership moves elsewhere (an assignment, a call argument, a field/collection store, a
// match-arm destructure, a return), which is what keeps a moved-away value from being dropped
// twice with no special-casing at any individual site.
struct IrDrop final : IrInst
{
    int value;
};

// Not currently emitted for a plain struct/enum (move semantics needs no runtime refcount - see
// IrDrop's own updated comment) - reserved for Shared<T>'s own explicit, opt-in refcounting, not
// yet built. LlvmIrEmitter recognizes but no-ops this instruction until then (see its own
// emitInstructions dispatch).
struct IrRetain final : IrInst
{
    int value;
};

// `while`/`loop`, kept structured like IrBranch: a nested conditionBlock
// (empty for infinite `loop`) and body instead of separate labeled blocks.
// `dest` (from IrInst) is the loop's own produced value - only meaningful
// for `loop` (used when consumed as an expression); `while` never produces
// one. See docs/language/0028-loops.md for the full design, in particular
// why loop-carried mutation is represented via `carried` (consumed by the
// LLVM backend as alloca/load/store, not phi nodes) rather than anything
// resembling strict SSA at this level.
struct IrLoop final : IrInst
{
    std::vector<std::unique_ptr<IrInst>> conditionBlock; // empty for infinite `loop`
    int conditionValue = -1; // register in conditionBlock; -1 = infinite
    std::vector<std::unique_ptr<IrInst>> body;
    // (register holding a name's value just before the loop, register
    // holding it at the end of one static body traversal) per name mutated
    // inside the loop body - populated by diffing an IrScope snapshot taken
    // before/after lowering body.
    std::vector<std::pair<int, int>> carried;
};

// `break [value]`, always targets the innermost enclosing IrLoop. `carried`
// mirrors IrLoop::carried but snapshotted at *this* point in the body rather
// than the body's natural end - a break can fire before any/all
// reassignments happen, so the LLVM backend needs to know exactly which
// carried variables changed (and to what) by the time control reaches here,
// to correctly update their storage before jumping to the loop's exit.
struct IrBreak final : IrInst
{
    int value = -1; // -1 = bare `break`
    std::vector<std::pair<int, int>> carried;
};

// `continue`, always targets the innermost enclosing IrLoop. `carried`: see
// IrBreak - same reasoning, needed before jumping back to the loop header.
struct IrContinue final : IrInst
{
    std::vector<std::pair<int, int>> carried;
};

struct IrFunction
{
    std::string name;
    std::vector<std::string> paramNames;
    std::vector<std::string> paramTypes; // declared type names, parallel to paramNames
    std::optional<std::string> returnType;
    std::vector<std::unique_ptr<IrInst>> body;
    int registerCount = 0;
};

// `extern c name(params) [-> returnType]` (see docs/language/0048-ffi.md)
// - no `body`/`registerCount`, unlike IrFunction: an extern declaration is
// never lowered itself, only *registered* so a call site resolves its
// signature; LlvmIrEmitter emits a `declare`, not a `define`, for each one.
struct IrExtern
{
    std::string name;
    std::vector<std::string> paramTypes; // declared type names only - no names needed,
                                         // extern params are never referenced by name
    std::optional<std::string> returnType;
};

struct IrProgram
{
    std::vector<IrFunction> functions;
    std::vector<IrExtern> externs;
    std::vector<std::unique_ptr<IrInst>> topLevel;
    // struct name -> its fields, in declared order, as (fieldName, fieldType) pairs.
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> structs;
    // struct name -> its `impl Display for <name>`'s own mangled `format`
    // function name (see docs/language/0062-display-trait.md), when one
    // was declared and passed TypeChecker's own conformance check. Only
    // ever populated for the one compiler-recognized trait ("Display")
    // that actually drives runtime stringification dispatch - a real
    // `impl SomeOtherTrait for X` still compiles its own methods as
    // ordinary functions (see `functions` above) but leaves this map
    // untouched, since nothing consumes any other trait name yet.
    std::unordered_map<std::string, std::string> displayImpls;
    // `enum` declarations (see docs/language/0064-enums.md) - an enum's own runtime
    // representation is stored in `structs` above too (the flattened `{i32 tag, <all variants'
    // own fields concatenated>}` layout - see IrGenerator::generate's own comment for why
    // reusing the struct machinery wholesale was worth it), but that alone can't tell
    // LlvmIrEmitter which struct-name entries are *really* enums (needing a tag-aware
    // print/tostring function, never the generic "print every field" one every ordinary struct
    // gets) versus real structs. This is that distinguishing registry: enum name -> each
    // variant's own (name, payload field count), in declared order - enough to recompute every
    // variant's own field-index range within the flattened struct (variant i's own fields start
    // right after the tag and every earlier variant's own field count).
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> enums;
};
