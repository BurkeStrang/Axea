#pragma once

#include "lexer/TokenKind.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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

// `List<elem>()` - a fresh, empty, growable list (see docs/language/0033-lists.md).
// `elementTypeName` is carried explicitly (unlike IrArrayNew, which infers it
// from its own elements) - a brand-new empty list has no elements to infer
// the type from.
struct IrListNew final : IrInst
{
    std::string elementTypeName;
};

// `list.push(value)` - no dest (void); mutates `list`'s own header fields in
// place. IndexGet/IndexSet/FieldGet(".length") are all reused unchanged for
// List (see docs/language/0033-lists.md) - only push/pop need dedicated
// instructions, since they're the only operations that change a list's shape
// rather than just reading/writing an existing slot.
struct IrListPush final : IrInst
{
    int list;
    int value;
};

// `list.pop()` - dest is the removed element.
struct IrListPop final : IrInst
{
    int list;
};

// `Stack<T>()` - a LIFO collection backed internally by List<T>'s own
// machinery (see docs/language/0035-stacks.md). Carries elementTypeName
// exactly like IrListNew - a brand-new empty stack has nothing to infer it
// from.
struct IrStackNew final : IrInst
{
    std::string elementTypeName;
};

// `stack.push(value)` - no dest (void); mirrors IrListPush exactly.
struct IrStackPush final : IrInst
{
    int stack;
    int value;
};

// `stack.pop()` - dest is the removed element; mirrors IrListPop exactly.
struct IrStackPop final : IrInst
{
    int stack;
};

// `stack.peek()` - dest is the top element, *not* removed (the one
// genuinely new operation List<T> doesn't have - see
// docs/language/0035-stacks.md).
struct IrStackPeek final : IrInst
{
    int stack;
};

// `LinkedList<elem>()` - a fresh, empty, doubly linked, node-based collection
// (see docs/language/0036-linked-lists.md). Carries elementTypeName exactly
// like IrListNew/IrStackNew - a brand-new empty list has nothing to infer it
// from.
struct IrLinkedListNew final : IrInst
{
    std::string elementTypeName;
};

// `list.push_front(value)`/`list.push_back(value)` - no dest (void); mutate
// `list`'s own header fields (and link a fresh node) in place.
struct IrLinkedListPushFront final : IrInst
{
    int list;
    int value;
};

struct IrLinkedListPushBack final : IrInst
{
    int list;
    int value;
};

// `list.pop_front()`/`list.pop_back()` - dest is the removed element.
struct IrLinkedListPopFront final : IrInst
{
    int list;
};

struct IrLinkedListPopBack final : IrInst
{
    int list;
};

// `Deque<elem>()` - a fresh, empty, growable array with a `start` offset
// (see docs/language/0037-deques.md). Carries elementTypeName exactly like
// IrListNew/IrStackNew/IrLinkedListNew - a brand-new empty deque has
// nothing to infer it from.
struct IrDequeNew final : IrInst
{
    std::string elementTypeName;
};

// `deque.push_front(value)`/`deque.push_back(value)` - no dest (void);
// mutate `deque`'s own header fields in place (reallocating - see
// docs/language/0037-deques.md).
struct IrDequePushFront final : IrInst
{
    int deque;
    int value;
};

struct IrDequePushBack final : IrInst
{
    int deque;
    int value;
};

// `deque.pop_front()`/`deque.pop_back()` - dest is the removed element (no
// reallocation - just start/count arithmetic).
struct IrDequePopFront final : IrInst
{
    int deque;
};

struct IrDequePopBack final : IrInst
{
    int deque;
};

// `Queue<elem>()` - a fresh, empty FIFO collection backed internally by
// Deque<T>'s own machinery (see docs/language/0038-queues.md). Carries
// elementTypeName exactly like IrDequeNew.
struct IrQueueNew final : IrInst
{
    std::string elementTypeName;
};

// `queue.enqueue(value)` - no dest (void); maps onto Deque<T>.push_back's
// own shape.
struct IrQueueEnqueue final : IrInst
{
    int queue;
    int value;
};

// `queue.dequeue()` - dest is the removed element; maps onto
// Deque<T>.pop_front's own shape.
struct IrQueueDequeue final : IrInst
{
    int queue;
};

// `PriorityQueue<elem>()` - a fresh, empty binary heap (see
// docs/language/0039-priority-queues.md). Carries elementTypeName exactly
// like IrListNew/IrStackNew - a brand-new empty heap has nothing to infer it
// from. `elementTypeName` is always "i32" in a well-typed program (the only
// orderable type this phase), but carried as a string anyway, mirroring
// every other collection's *New instruction here.
struct IrPriorityQueueNew final : IrInst
{
    std::string elementTypeName;
};

// `priorityQueue.push(value)` - no dest (void); appends then sifts the new
// element up toward the root until the heap property holds again.
struct IrPriorityQueuePush final : IrInst
{
    int priorityQueue;
    int value;
};

// `priorityQueue.pop()` - dest is the removed minimum; moves the last
// element into the vacated root slot, then sifts it down until the heap
// property holds again.
struct IrPriorityQueuePop final : IrInst
{
    int priorityQueue;
};

// `priorityQueue.peek()` - dest is the minimum element, *not* removed - the
// minimum always sits at index 0 by the heap invariant, so (unlike
// IrStackPeek) this needs no arithmetic at all.
struct IrPriorityQueuePeek final : IrInst
{
    int priorityQueue;
};

// `SortedMap<key,value>()` - a fresh, empty AVL tree (see
// docs/language/0040-sorted-maps.md). Carries the concrete K/V type strings
// explicitly, exactly like IrMapNew - a brand-new empty tree has nothing to
// infer them from, and LlvmIrEmitter needs them to look up (or register, on
// first sight) the right monomorphized instantiation.
struct IrSortedMapNew final : IrInst
{
    std::string keyTypeName;
    std::string valueTypeName;
};

// `sortedMap.set(key, value)` - no dest (unit); inserts (with AVL
// rebalancing) or updates in place.
struct IrSortedMapSet final : IrInst
{
    int sortedMap;
    int key;
    int value;
};

// `sortedMap.get(key)` - dest is the value (or an unspecified sentinel if
// the key is absent in compiled code, mirroring IrMapGet; the interpreter
// throws instead).
struct IrSortedMapGet final : IrInst
{
    int sortedMap;
    int key;
};

// `sortedMap.contains(key)` - dest is a bool.
struct IrSortedMapContains final : IrInst
{
    int sortedMap;
    int key;
};

// `sortedMap.remove(key)` - no dest (unit); no-op if the key is absent.
// Removing (with AVL rebalancing) mirrors IrMapRemove's own shape.
struct IrSortedMapRemove final : IrInst
{
    int sortedMap;
    int key;
};

// `SortedSet<elem>()` - a fresh, empty AVL tree (see
// docs/language/0041-sorted-sets.md). Carries elementTypeName exactly like
// IrSetNew - a brand-new empty tree has nothing to infer it from.
struct IrSortedSetNew final : IrInst
{
    std::string elementTypeName;
};

// `sortedSet.add(value)` - no dest (unit); inserts (with AVL rebalancing)
// or no-ops if already present, mirroring IrSetAdd's own shape.
struct IrSortedSetAdd final : IrInst
{
    int sortedSet;
    int value;
};

// `sortedSet.contains(value)` - dest is a bool.
struct IrSortedSetContains final : IrInst
{
    int sortedSet;
    int value;
};

// `sortedSet.remove(value)` - no dest (unit); no-op if absent. Removing
// (with AVL rebalancing) mirrors IrSetRemove's own shape.
struct IrSortedSetRemove final : IrInst
{
    int sortedSet;
    int value;
};

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
// no value register to infer from (mirrors IrListNew's own
// elementTypeName, needed for the identical reason on an empty list).
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

// `Map<K,V>()` - a fresh, empty hash table (see docs/language/0034-maps-and-sets.md's
// generic rewrite). Carries the concrete K/V type strings explicitly - like
// IrListNew's own elementTypeName, a brand-new empty Map has nothing to infer
// them from - so LlvmIrEmitter can look up (or register, on first sight) the
// right monomorphized instantiation.
struct IrMapNew final : IrInst
{
    std::string keyTypeName;
    std::string valueTypeName;
};

// `map.set(key, value)` - no dest (unit); inserts or updates in place.
struct IrMapSet final : IrInst
{
    int map;
    int key;
    int value;
};

// `map.get(key)` - dest is the value (or an unspecified sentinel if the key
// is absent in compiled code; the interpreter throws instead - see
// docs/language/0034-maps-and-sets.md).
struct IrMapGet final : IrInst
{
    int map;
    int key;
};

// `map.contains(key)` - dest is a bool.
struct IrMapContains final : IrInst
{
    int map;
    int key;
};

// `map.remove(key)` - no dest (unit); no-op if the key is absent.
struct IrMapRemove final : IrInst
{
    int map;
    int key;
};

// `Set<T>()` - a fresh, empty hash set. Same reasoning as IrMapNew above.
struct IrSetNew final : IrInst
{
    std::string elementTypeName;
};

// `set.add(value)` - no dest (unit); no-op if already present.
struct IrSetAdd final : IrInst
{
    int set;
    int value;
};

// `set.contains(value)` - dest is a bool.
struct IrSetContains final : IrInst
{
    int set;
    int value;
};

// `set.remove(value)` - no dest (unit); no-op if absent.
struct IrSetRemove final : IrInst
{
    int set;
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
    // (name, final register) for each top-level assignment, in source order -
    // mirrors how `ax run` reports top-level bindings, and is what a
    // generated `main` (LlvmIrEmitter) prints.
    std::vector<std::pair<std::string, int>> topLevelBindings;
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
