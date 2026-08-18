#include "sema/TypeChecker.hpp"

#include <stdexcept>

namespace
{
    // Builds a Type with only `kind`/`structName` set, value-initializing
    // every other field (elementKind/elementStructName/arraySize/
    // elementTypeName/valueTypeName) - avoids -Wmissing-field-initializers,
    // which (unlike leaving every field default via `Type{}`) does fire on
    // any *partial* aggregate init, no matter how many trailing fields are
    // provided.
    Type simpleType(TypeKind kind, std::string structName = "")
    {
        Type type{};
        type.kind = kind;
        type.structName = std::move(structName);
        return type;
    }

    // Same reasoning as simpleType above, for Array/Slice/List's flat
    // one-level element representation.
    Type arrayLikeType(TypeKind kind,
                       TypeKind elementKind,
                       std::string elementStructName,
                       int arraySize = 0)
    {
        Type type{};
        type.kind = kind;
        type.elementKind = elementKind;
        type.elementStructName = std::move(elementStructName);
        type.arraySize = arraySize;
        return type;
    }

    const Type kBool = simpleType(TypeKind::Bool);
    const Type kI32 = simpleType(TypeKind::I32);
    const Type kChar = simpleType(TypeKind::Char);
    const Type kUnit = simpleType(TypeKind::Unit);
    const Type kStr = simpleType(TypeKind::String);

    void requireInt(const Type& left, const Type& right)
    {
        if (!(left == kI32) || !(right == kI32))
        {
            throw std::runtime_error("arithmetic/comparison requires i32 operands, found " +
                                     typeName(left) + " and " + typeName(right));
        }
    }

    // Ordering (`<`/`<=`/`>`/`>=`) - unlike arithmetic (requireInt above,
    // i32-only), also accepts char, since a Unicode scalar value has a
    // natural total order by codepoint (see docs/language/0044-char.md).
    // A char is never itself accepted in requireInt - char + char, unlike
    // char < char, is deliberately still an error.
    void requireOrdered(const Type& left, const Type& right)
    {
        const bool bothInt = left == kI32 && right == kI32;
        const bool bothChar = left == kChar && right == kChar;
        if (!bothInt && !bothChar)
        {
            throw std::runtime_error("comparison requires two i32 or two char operands, found " +
                                     typeName(left) + " and " + typeName(right));
        }
    }

    // True for anything that can stand in for a `str` argument: an actual
    // `str`, or an owned `String` (see docs/language/0042-string.md and
    // docs/std/strings/0001-str.md's own "Passing String automatically
    // lends a str" framing) - mirrors the array-to-slice coercion rule
    // arrayToSliceCoercion below already established for a different pair
    // of types, at a different site (a call boundary, not every str-typed
    // context - String never implicitly appears where a wider owned type
    // is expected, only the reverse).
    bool isStrCoercible(const Type& type)
    {
        return type == kStr || type.kind == TypeKind::OwnedString;
    }

    // Shared by IndexExpr and IndexAssignStmt: a literal (compile-time-known)
    // index can be range-checked right now, without waiting for a runtime
    // check (see docs/language/0031-arrays.md). A non-literal index is only
    // checked at runtime, by the interpreter - mirrors how division by zero
    // is checked there but not here.
    void checkLiteralIndexBounds(const Expr& indexExpr, const Type& arrayType)
    {
        const auto* literalIndex = dynamic_cast<const IntegerExpr*>(&indexExpr);
        if (!literalIndex)
        {
            return;
        }
        if (literalIndex->value < 0 || literalIndex->value >= arrayType.arraySize)
        {
            throw std::runtime_error("array index " + std::to_string(literalIndex->value) +
                                     " out of bounds for array of size " +
                                     std::to_string(arrayType.arraySize));
        }
    }

    bool isIndexable(const Type& type)
    {
        // Deque<T> (see docs/language/0037-deques.md) joins Array/Slice/List
        // here - unlike Stack<T>/LinkedList<T>, its growable-array-with-a-
        // start-offset representation genuinely supports O(1) random access,
        // so `[i]`/`[i]=`/`for`-in all work through this exact same shared
        // path with zero further IndexExpr/IndexAssignStmt-specific code.
        return type.kind == TypeKind::Array || type.kind == TypeKind::Slice ||
               type.kind == TypeKind::List || type.kind == TypeKind::Deque;
    }

    // slice<T> is deliberately scoped to function parameters only this phase
    // (see docs/language/0032-slices.md) - a slice value never needs to
    // survive past a single call, which is what keeps RegionChecker untouched
    // and avoids needing slice support in ax run's top-level-binding printer.
    // Called at every type-resolution site that is *not* a parameter.
    void rejectSliceOutsideParameter(const Type& type, const std::string& context)
    {
        if (type.kind == TypeKind::Slice)
        {
            throw std::runtime_error("slice<T> is only supported as a function parameter type, "
                                     "not as " +
                                     context);
        }
    }

    // Unlike slice<T>, List<T> is a real owned heap value (like struct/array)
    // and may freely be a parameter, return type, or local declared type -
    // it's only rejected as a struct field type this phase, purely to keep
    // this pass's surface area bounded, not for any deeper architectural
    // reason (see docs/language/0033-lists.md).
    void rejectListAsFieldType(const Type& type)
    {
        if (type.kind == TypeKind::List)
        {
            throw std::runtime_error(
                "List<T> is not supported as a struct field type in this phase");
        }
    }

    // Same "kept restricted purely to bound this pass's surface area, not
    // for any deeper architectural reason" rationale as
    // rejectListAsFieldType above - see docs/language/0034-maps-and-sets.md.
    void rejectMapOrSetAsFieldType(const Type& type)
    {
        if (type.kind == TypeKind::Map || type.kind == TypeKind::Set)
        {
            throw std::runtime_error(typeName(type) +
                                     " is not supported as a struct field type in this phase");
        }
    }

    // Same rationale again - see docs/language/0035-stacks.md.
    void rejectStackAsFieldType(const Type& type)
    {
        if (type.kind == TypeKind::Stack)
        {
            throw std::runtime_error(
                "Stack<T> is not supported as a struct field type in this phase");
        }
    }

    // Same rationale again - see docs/language/0036-linked-lists.md.
    void rejectLinkedListAsFieldType(const Type& type)
    {
        if (type.kind == TypeKind::LinkedList)
        {
            throw std::runtime_error(
                "LinkedList<T> is not supported as a struct field type in this phase");
        }
    }

    // Same rationale again - see docs/language/0037-deques.md.
    void rejectDequeAsFieldType(const Type& type)
    {
        if (type.kind == TypeKind::Deque)
        {
            throw std::runtime_error(
                "Deque<T> is not supported as a struct field type in this phase");
        }
    }

    // Same rationale again - see docs/language/0038-queues.md.
    void rejectQueueAsFieldType(const Type& type)
    {
        if (type.kind == TypeKind::Queue)
        {
            throw std::runtime_error(
                "Queue<T> is not supported as a struct field type in this phase");
        }
    }

    // Same rationale again - see docs/language/0039-priority-queues.md.
    void rejectPriorityQueueAsFieldType(const Type& type)
    {
        if (type.kind == TypeKind::PriorityQueue)
        {
            throw std::runtime_error(
                "PriorityQueue<T> is not supported as a struct field type in this phase");
        }
    }

    // Same rationale again - see docs/language/0040-sorted-maps.md.
    void rejectSortedMapAsFieldType(const Type& type)
    {
        if (type.kind == TypeKind::SortedMap)
        {
            throw std::runtime_error(
                "SortedMap<K,V> is not supported as a struct field type in this phase");
        }
    }

    // Same rationale again - see docs/language/0041-sorted-sets.md.
    void rejectSortedSetAsFieldType(const Type& type)
    {
        if (type.kind == TypeKind::SortedSet)
        {
            throw std::runtime_error(
                "SortedSet<T> is not supported as a struct field type in this phase");
        }
    }

    // Same rationale again - see docs/language/0042-string.md.
    void rejectOwnedStringAsFieldType(const Type& type)
    {
        if (type.kind == TypeKind::OwnedString)
        {
            throw std::runtime_error(
                "String is not supported as a struct field type in this phase");
        }
    }

    // Same rationale again - see docs/language/0043-buffer.md.
    void rejectBufferAsFieldType(const Type& type)
    {
        if (type.kind == TypeKind::Buffer)
        {
            throw std::runtime_error(
                "Buffer is not supported as a struct field type in this phase");
        }
    }

    // Finds the first *top-level* comma in `text` - i.e. one not nested
    // inside a further `<...>`/`[...]` type argument list (e.g. a Map value
    // type that's itself `Map<i32,i32>`, or `[i32;4]`'s own bracket pair).
    // Needed the instant Map/Set's K/V can be arbitrarily nested (see
    // docs/language/0034-maps-and-sets.md's generic rewrite) - the naive
    // `text.find(',')` this replaces broke as soon as a value type could
    // itself contain a comma.
    std::size_t findTopLevelComma(const std::string& text)
    {
        int depth = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '<' || text[i] == '[')
            {
                ++depth;
            }
            else if (text[i] == '>' || text[i] == ']')
            {
                --depth;
            }
            else if (text[i] == ',' && depth == 0)
            {
                return i;
            }
        }
        return std::string::npos;
    }
} // namespace

std::string typeName(const Type& type)
{
    switch (type.kind)
    {
        case TypeKind::Bool: return "bool";
        case TypeKind::I32: return "i32";
        case TypeKind::Char: return "char";
        case TypeKind::String: return "str";
        case TypeKind::Unit: return "unit";
        case TypeKind::Struct: return type.structName;
        case TypeKind::Array:
        {
            const Type element = simpleType(type.elementKind, type.elementStructName);
            return "[" + typeName(element) + "; " + std::to_string(type.arraySize) + "]";
        }
        case TypeKind::Slice:
        {
            const Type element = simpleType(type.elementKind, type.elementStructName);
            return "slice<" + typeName(element) + ">";
        }
        case TypeKind::List:
        {
            const Type element = simpleType(type.elementKind, type.elementStructName);
            return "List<" + typeName(element) + ">";
        }
        case TypeKind::Stack:
        {
            const Type element = simpleType(type.elementKind, type.elementStructName);
            return "Stack<" + typeName(element) + ">";
        }
        case TypeKind::LinkedList:
        {
            const Type element = simpleType(type.elementKind, type.elementStructName);
            return "LinkedList<" + typeName(element) + ">";
        }
        case TypeKind::Deque:
        {
            const Type element = simpleType(type.elementKind, type.elementStructName);
            return "Deque<" + typeName(element) + ">";
        }
        case TypeKind::Queue:
        {
            const Type element = simpleType(type.elementKind, type.elementStructName);
            return "Queue<" + typeName(element) + ">";
        }
        case TypeKind::PriorityQueue:
        {
            const Type element = simpleType(type.elementKind, type.elementStructName);
            return "PriorityQueue<" + typeName(element) + ">";
        }
        // elementTypeName/valueTypeName are always already-canonical strings
        // (see docs/language/0034-maps-and-sets.md's generic rewrite), so
        // reconstruction is trivial - unlike Array/Slice/List's flat
        // elementKind tag, there's no nested Type to recurse into here.
        case TypeKind::Map: return "Map<" + type.elementTypeName + "," + type.valueTypeName + ">";
        case TypeKind::Set: return "Set<" + type.elementTypeName + ">";
        case TypeKind::SortedMap:
            return "SortedMap<" + type.elementTypeName + "," + type.valueTypeName + ">";
        case TypeKind::SortedSet: return "SortedSet<" + type.elementTypeName + ">";
        case TypeKind::OwnedString: return "String";
        case TypeKind::Buffer: return "Buffer";
        default: return "<unsupported type>";
    }
}

TypeEnv::TypeEnv(const TypeEnv* parent)
    : parent_(parent)
{
}

void TypeEnv::define(const std::string& name, Type type)
{
    types_[name] = std::move(type);
}

Type TypeEnv::get(const std::string& name) const
{
    if (const auto it = types_.find(name); it != types_.end())
    {
        return it->second;
    }
    if (parent_)
    {
        return parent_->get(name);
    }
    throw std::runtime_error("undefined variable: " + name);
}

Type TypeChecker::resolveType(const std::string& name) const
{
    static const std::unordered_map<std::string, TypeKind> primitives{
        {"bool", TypeKind::Bool},
        {"i32", TypeKind::I32},
        {"char", TypeKind::Char},
        {"str", TypeKind::String},
        {"unit", TypeKind::Unit},
    };

    // "[elem;N]" - the canonical (no-spaces) form Parser::parseTypeName
    // always produces (see docs/language/0031-arrays.md).
    if (!name.empty() && name.front() == '[')
    {
        const auto semicolon = name.find(';');
        const auto closeBracket = name.rfind(']');
        if (semicolon == std::string::npos || closeBracket == std::string::npos)
        {
            throw std::runtime_error("malformed array type: " + name);
        }

        const std::string elementName = name.substr(1, semicolon - 1);
        const std::string sizeText = name.substr(semicolon + 1, closeBracket - semicolon - 1);
        const Type elementType = resolveType(elementName); // one level deep only - no nested arrays
        if (elementType.kind == TypeKind::Array)
        {
            throw std::runtime_error("nested array types are not supported: " + name);
        }

        return arrayLikeType(
            TypeKind::Array, elementType.kind, elementType.structName, std::stoi(sizeText));
    }

    // "slice<elem>" - the canonical form Parser::parseTypeName always
    // produces (see docs/language/0032-slices.md).
    if (name.starts_with("slice<") && name.back() == '>')
    {
        const std::string elementName = name.substr(6, name.size() - 7);
        const Type elementType = resolveType(elementName); // one level deep only - no nested slices
        if (elementType.kind == TypeKind::Array || elementType.kind == TypeKind::Slice)
        {
            throw std::runtime_error("nested array/slice element types are not supported: " + name);
        }

        return arrayLikeType(TypeKind::Slice, elementType.kind, elementType.structName);
    }

    // "List<elem>" - the canonical form Parser::parseTypeName always
    // produces (see docs/language/0033-lists.md).
    if (name.starts_with("List<") && name.back() == '>')
    {
        const std::string elementName = name.substr(5, name.size() - 6);
        const Type elementType = resolveType(elementName); // one level deep only - no nested lists
        if (elementType.kind == TypeKind::Array || elementType.kind == TypeKind::Slice ||
            elementType.kind == TypeKind::List || elementType.kind == TypeKind::Stack ||
            elementType.kind == TypeKind::LinkedList || elementType.kind == TypeKind::Deque ||
            elementType.kind == TypeKind::Queue || elementType.kind == TypeKind::PriorityQueue)
        {
            throw std::runtime_error(
                "nested array/slice/List/Stack/LinkedList/Deque/Queue/PriorityQueue element "
                "types are not supported: " +
                name);
        }

        return arrayLikeType(TypeKind::List, elementType.kind, elementType.structName);
    }

    // "Stack<elem>" - a LIFO collection backed internally by List<T>'s own
    // machinery (see docs/language/0035-stacks.md) - same one-level element
    // restriction as List<elem> above, for the same reason (and extended to
    // also reject a nested Stack<Stack<T>>, for symmetry).
    if (name.starts_with("Stack<") && name.back() == '>')
    {
        const std::string elementName = name.substr(6, name.size() - 7);
        const Type elementType = resolveType(elementName);
        if (elementType.kind == TypeKind::Array || elementType.kind == TypeKind::Slice ||
            elementType.kind == TypeKind::List || elementType.kind == TypeKind::Stack ||
            elementType.kind == TypeKind::LinkedList || elementType.kind == TypeKind::Deque ||
            elementType.kind == TypeKind::Queue || elementType.kind == TypeKind::PriorityQueue)
        {
            throw std::runtime_error(
                "nested array/slice/List/Stack/LinkedList/Deque/Queue/PriorityQueue element "
                "types are not supported: " +
                name);
        }

        return arrayLikeType(TypeKind::Stack, elementType.kind, elementType.structName);
    }

    // "LinkedList<elem>" - a doubly linked, node-based collection (see
    // docs/language/0036-linked-lists.md) - same one-level element
    // restriction as List<elem>/Stack<elem> above, for the same reason.
    if (name.starts_with("LinkedList<") && name.back() == '>')
    {
        const std::string elementName = name.substr(11, name.size() - 12);
        const Type elementType = resolveType(elementName);
        if (elementType.kind == TypeKind::Array || elementType.kind == TypeKind::Slice ||
            elementType.kind == TypeKind::List || elementType.kind == TypeKind::Stack ||
            elementType.kind == TypeKind::LinkedList || elementType.kind == TypeKind::Deque ||
            elementType.kind == TypeKind::Queue || elementType.kind == TypeKind::PriorityQueue)
        {
            throw std::runtime_error(
                "nested array/slice/List/Stack/LinkedList/Deque/Queue/PriorityQueue element "
                "types are not supported: " +
                name);
        }

        return arrayLikeType(TypeKind::LinkedList, elementType.kind, elementType.structName);
    }

    // "Deque<elem>" - a growable array with a `start` offset, supporting
    // real O(1) indexing (see docs/language/0037-deques.md) - same
    // one-level element restriction as List<elem>/Stack<elem>/
    // LinkedList<elem> above, for the same reason.
    if (name.starts_with("Deque<") && name.back() == '>')
    {
        const std::string elementName = name.substr(6, name.size() - 7);
        const Type elementType = resolveType(elementName);
        if (elementType.kind == TypeKind::Array || elementType.kind == TypeKind::Slice ||
            elementType.kind == TypeKind::List || elementType.kind == TypeKind::Stack ||
            elementType.kind == TypeKind::LinkedList || elementType.kind == TypeKind::Deque ||
            elementType.kind == TypeKind::Queue || elementType.kind == TypeKind::PriorityQueue)
        {
            throw std::runtime_error(
                "nested array/slice/List/Stack/LinkedList/Deque/Queue/PriorityQueue element "
                "types are not supported: " +
                name);
        }

        return arrayLikeType(TypeKind::Deque, elementType.kind, elementType.structName);
    }

    // "Queue<elem>" - a FIFO collection backed internally by Deque<T>'s own
    // machinery (see docs/language/0038-queues.md) - same one-level element
    // restriction as List<elem>/Stack<elem>/LinkedList<elem>/Deque<elem>
    // above, for the same reason.
    if (name.starts_with("Queue<") && name.back() == '>')
    {
        const std::string elementName = name.substr(6, name.size() - 7);
        const Type elementType = resolveType(elementName);
        if (elementType.kind == TypeKind::Array || elementType.kind == TypeKind::Slice ||
            elementType.kind == TypeKind::List || elementType.kind == TypeKind::Stack ||
            elementType.kind == TypeKind::LinkedList || elementType.kind == TypeKind::Deque ||
            elementType.kind == TypeKind::Queue || elementType.kind == TypeKind::PriorityQueue)
        {
            throw std::runtime_error(
                "nested array/slice/List/Stack/LinkedList/Deque/Queue/PriorityQueue element "
                "types are not supported: " +
                name);
        }

        return arrayLikeType(TypeKind::Queue, elementType.kind, elementType.structName);
    }

    // "PriorityQueue<elem>" - a real binary heap (see
    // docs/language/0039-priority-queues.md). Unlike List/Stack/LinkedList/
    // Deque/Queue above, this is a real *semantic* restriction, not a
    // structural nesting-depth one - `<`/`<=`/`>`/`>=` (requireInt above)
    // only ever typecheck for i32 operands, so i32 is, today, the only type
    // in this language a heap can meaningfully order at all. Mirrors how
    // Set<T>/Map<K,V> below call isHashable rather than a structural check,
    // for the identical reason: "some types don't qualify, and the reason
    // is domain-specific, not nesting depth."
    if (name.starts_with("PriorityQueue<") && name.back() == '>')
    {
        const std::string elementName = name.substr(14, name.size() - 15);
        const Type elementType = resolveType(elementName);
        if (elementType.kind != TypeKind::I32)
        {
            throw std::runtime_error(
                "PriorityQueue<T> requires an orderable element type (i32 only in this phase - "
                "no other type is comparable yet), found PriorityQueue<" +
                typeName(elementType) + ">");
        }

        return arrayLikeType(TypeKind::PriorityQueue, elementType.kind, elementType.structName);
    }

    // "Set<elem>" - the canonical form Parser::parseTypeName always produces
    // (see docs/language/0034-maps-and-sets.md's generic rewrite, modeled on
    // Rust: the element type needs real Hash+Eq, monomorphized per distinct
    // shape actually used - see LlvmIrEmitter). `elementTypeName` stores the
    // canonical string (`typeName` of the resolved element type, not the raw
    // source text) so two differently-spelled-but-equivalent Set<T>s still
    // compare equal via the defaulted operator==.
    if (name.starts_with("Set<") && name.back() == '>')
    {
        const std::string elementName = name.substr(4, name.size() - 5);
        const Type elementType = resolveType(elementName);
        rejectSliceOutsideParameter(elementType, "a Set element type");
        std::unordered_set<std::string> visitedStructs;
        if (!isHashable(elementType, visitedStructs))
        {
            throw std::runtime_error(
                "Set<T> requires a hashable element type (i32, bool, str, or a struct/array/"
                "List composed entirely of hashable types), found Set<" +
                typeName(elementType) + ">");
        }
        Type result{};
        result.kind = TypeKind::Set;
        result.elementTypeName = typeName(elementType);
        return result;
    }

    // "SortedSet<elem>" - a real AVL tree, keeping elements ordered (see
    // docs/language/0041-sorted-sets.md). Same string-based
    // elementTypeName representation Set<T> above uses (not List/Stack's
    // own flat elementKind - mirrors Set<T>, not the array-like
    // collections, since SortedSet<T> is "Set<T>, but ordered"). The
    // restriction here is orderability, not hashability - same i32-only
    // requirement PriorityQueue<T>/SortedMap<K,V>'s own element/key already
    // have (see docs/language/0039-priority-queues.md), since `<`/`<=`/
    // `>`/`>=` only ever typecheck for i32 operands today.
    if (name.starts_with("SortedSet<") && name.back() == '>')
    {
        const std::string elementName = name.substr(10, name.size() - 11);
        const Type elementType = resolveType(elementName);
        if (elementType.kind != TypeKind::I32)
        {
            throw std::runtime_error(
                "SortedSet<T> requires an orderable element type (i32 only in this phase - no "
                "other type is comparable yet), found SortedSet<" +
                typeName(elementType) + ">");
        }
        Type result{};
        result.kind = TypeKind::SortedSet;
        result.elementTypeName = typeName(elementType);
        return result;
    }

    // "Map<key,value>" - the canonical form Parser::parseTypeName always
    // produces. Same Hash+Eq requirement on the key as Set<T> above; V has no
    // such requirement (never hashed or compared - only ever stored) and may
    // be any resolvable type except slice<T> (still parameter-only).
    if (name.starts_with("Map<") && name.back() == '>')
    {
        const std::string args = name.substr(4, name.size() - 5);
        const auto comma = findTopLevelComma(args);
        if (comma == std::string::npos)
        {
            throw std::runtime_error("malformed Map type: " + name);
        }
        const std::string keyName = args.substr(0, comma);
        const std::string valueName = args.substr(comma + 1);
        const Type keyType = resolveType(keyName);
        const Type valueType = resolveType(valueName);
        rejectSliceOutsideParameter(keyType, "a Map key type");
        rejectSliceOutsideParameter(valueType, "a Map value type");
        std::unordered_set<std::string> visitedStructs;
        if (!isHashable(keyType, visitedStructs))
        {
            throw std::runtime_error(
                "Map<K,V> requires a hashable key type (i32, bool, str, or a struct/array/List "
                "composed entirely of hashable types), found Map<" +
                typeName(keyType) + "," + typeName(valueType) + ">");
        }
        Type result{};
        result.kind = TypeKind::Map;
        result.elementTypeName = typeName(keyType);
        result.valueTypeName = typeName(valueType);
        return result;
    }

    // "SortedMap<key,value>" - a real AVL tree, keeping keys ordered (see
    // docs/language/0040-sorted-maps.md). Same two-type-argument shape as
    // Map<K,V> above, reusing its bracket-depth-aware comma split - but the
    // key restriction is orderability, not hashability: `<`/`<=`/`>`/`>=`
    // (requireInt above) only ever typecheck for i32 operands, so i32 is,
    // today, the only key type a tree can meaningfully order at all (same
    // reasoning PriorityQueue<T>'s own element-type restriction already
    // established - see docs/language/0039-priority-queues.md). V has no
    // such requirement (never compared, only stored) and may be any
    // resolvable type except slice<T> (still parameter-only), exactly like
    // Map<K,V>'s own V.
    if (name.starts_with("SortedMap<") && name.back() == '>')
    {
        const std::string args = name.substr(10, name.size() - 11);
        const auto comma = findTopLevelComma(args);
        if (comma == std::string::npos)
        {
            throw std::runtime_error("malformed SortedMap type: " + name);
        }
        const std::string keyName = args.substr(0, comma);
        const std::string valueName = args.substr(comma + 1);
        const Type keyType = resolveType(keyName);
        const Type valueType = resolveType(valueName);
        rejectSliceOutsideParameter(valueType, "a SortedMap value type");
        if (keyType.kind != TypeKind::I32)
        {
            throw std::runtime_error(
                "SortedMap<K,V> requires an orderable key type (i32 only in this phase - no "
                "other type is comparable yet), found SortedMap<" +
                typeName(keyType) + "," + typeName(valueType) + ">");
        }
        Type result{};
        result.kind = TypeKind::SortedMap;
        result.elementTypeName = typeName(keyType);
        result.valueTypeName = typeName(valueType);
        return result;
    }

    // "String" - Axea's own owned, growable byte buffer (see
    // docs/language/0042-string.md), distinct from the `str` primitive
    // above (TypeKind::String) despite the confusingly similar name -
    // `String` isn't generic (no `<...>` parameter, unlike every
    // collection above), so this is a bare exact-name match, not a
    // starts_with check.
    if (name == "String")
    {
        return simpleType(TypeKind::OwnedString);
    }

    // "Buffer" - Axea's own mutable, amortized-growth text-construction
    // type (see docs/language/0043-buffer.md) - same bare exact-name match
    // as "String" above, for the same reason (not generic).
    if (name == "Buffer")
    {
        return simpleType(TypeKind::Buffer);
    }

    if (const auto it = primitives.find(name); it != primitives.end())
    {
        return simpleType(it->second);
    }
    if (structs_.contains(name))
    {
        return simpleType(TypeKind::Struct, name);
    }
    throw std::runtime_error("unsupported type: " + name);
}

bool TypeChecker::isHashable(const Type& type,
                             std::unordered_set<std::string>& visitedStructs) const
{
    switch (type.kind)
    {
        case TypeKind::I32:
        case TypeKind::Bool:
        case TypeKind::String: return true;

        // Array/List/Stack all use the flat elementKind/elementStructName
        // representation (untouched by this phase's Map/Set rewrite) - so
        // their element's Type is directly reconstructible, no re-resolve
        // needed. Stack<T> is hashable under the same rule as List<T> - it's
        // backed by the identical mechanism (see docs/language/0035-stacks.md).
        case TypeKind::Array:
        case TypeKind::List:
        case TypeKind::Stack:
        {
            const Type element = simpleType(type.elementKind, type.elementStructName);
            return isHashable(element, visitedStructs);
        }

        case TypeKind::Struct:
        {
            // A self-/mutually-recursive struct chain is already possible
            // today (structs are always by-pointer) - revisiting a struct
            // still being checked means "not provably hashable", not an
            // infinite loop.
            if (!visitedStructs.insert(type.structName).second)
            {
                return false;
            }
            const StructDecl& decl = *structs_.at(type.structName);
            for (const auto& field : decl.fields)
            {
                if (!isHashable(resolveType(field.type), visitedStructs))
                {
                    return false;
                }
            }
            return true;
        }

        // Map, Set, slice, unit, and everything else: not hashable (mirrors
        // Rust - HashMap/HashSet don't implement Hash themselves either, no
        // canonical order to hash over).
        default: return false;
    }
}

void TypeChecker::registerSignatures(const Program& program)
{
    for (const auto& item : program.items)
    {
        if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get()))
        {
            functions_[function->name] = function;
        }
        else if (const auto* structDecl = dynamic_cast<const StructDecl*>(item.get()))
        {
            structs_[structDecl->name] = structDecl;
        }
    }

    // Validated in a second pass, once every struct name is known, so
    // structs/functions may reference each other regardless of order.
    for (const auto& [name, function] : functions_)
    {
        for (const auto& param : function->params)
        {
            resolveType(param.type); // slice<T> is allowed here - it's a parameter
        }
        if (function->returnType)
        {
            rejectSliceOutsideParameter(resolveType(*function->returnType),
                                        "a function return type");
        }
    }
    for (const auto& [name, structDecl] : structs_)
    {
        for (const auto& field : structDecl->fields)
        {
            const Type fieldType = resolveType(field.type);
            rejectSliceOutsideParameter(fieldType, "a struct field type");
            rejectListAsFieldType(fieldType);
            rejectMapOrSetAsFieldType(fieldType);
            rejectStackAsFieldType(fieldType);
            rejectLinkedListAsFieldType(fieldType);
            rejectDequeAsFieldType(fieldType);
            rejectQueueAsFieldType(fieldType);
            rejectPriorityQueueAsFieldType(fieldType);
            rejectSortedMapAsFieldType(fieldType);
            rejectSortedSetAsFieldType(fieldType);
            rejectOwnedStringAsFieldType(fieldType);
            rejectBufferAsFieldType(fieldType);
        }
    }
}

void TypeChecker::check(const Program& program)
{
    registerSignatures(program);

    TypeEnv globalEnv;
    for (const auto& item : program.items)
    {
        if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get()))
        {
            checkFunction(*function);
        }
        else if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(item.get()))
        {
            checkStmt(*assignment, globalEnv, nullptr, nullptr);
        }
    }
}

void TypeChecker::checkFunction(const FunctionDecl& function)
{
    TypeEnv env; // no parent: functions don't see top-level globals
    for (const auto& param : function.params)
    {
        env.define(param.name, resolveType(param.type));
    }

    const Type expectedReturn = function.returnType ? resolveType(*function.returnType) : kUnit;
    const auto& block = static_cast<const BlockExpr&>(*function.body);
    checkBlock(block,
               env,
               &expectedReturn,
               nullptr); // validates every return's value type, and the internal
                         // correctness of any leftover discarded trailing expression

    if (!(expectedReturn == kUnit) && !definitelyReturns(block))
    {
        throw std::runtime_error("function '" + function.name +
                                 "' does not return a value of type " + typeName(expectedReturn) +
                                 " on all paths (did you forget 'return'?)");
    }
}

bool TypeChecker::definitelyReturns(const BlockExpr& block) const
{
    for (const auto& statement : block.statements)
    {
        if (dynamic_cast<const ReturnStmt*>(statement.get()))
        {
            return true;
        }
        if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(statement.get()))
        {
            if (const auto* ifExpr = dynamic_cast<const IfExpr*>(exprStmt->expr.get());
                ifExpr && definitelyReturnsBranch(*ifExpr))
            {
                return true;
            }
        }
    }
    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(block.result.get()))
    {
        return definitelyReturnsBranch(*ifExpr);
    }
    return false;
}

bool TypeChecker::definitelyReturnsBranch(const IfExpr& ifExpr) const
{
    const auto& thenBlock = static_cast<const BlockExpr&>(*ifExpr.thenBranch);
    const auto& elseBlock = static_cast<const BlockExpr&>(*ifExpr.elseBranch);
    return definitelyReturns(thenBlock) && definitelyReturns(elseBlock);
}

Type TypeChecker::checkBlock(const BlockExpr& block,
                             TypeEnv& parentEnv,
                             const Type* expectedReturnType,
                             std::vector<Type>* currentLoopBreakTypes)
{
    TypeEnv env(&parentEnv);
    for (const auto& statement : block.statements)
    {
        checkStmt(*statement, env, expectedReturnType, currentLoopBreakTypes);
    }
    if (block.result)
    {
        return checkExpr(*block.result, env, expectedReturnType, currentLoopBreakTypes);
    }
    return kUnit;
}

void TypeChecker::checkStmt(const Stmt& stmt,
                            TypeEnv& env,
                            const Type* expectedReturnType,
                            std::vector<Type>* currentLoopBreakTypes)
{
    if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(&stmt))
    {
        const Type valueType =
            checkExpr(*assignment->value, env, expectedReturnType, currentLoopBreakTypes);
        if (assignment->declaredType)
        {
            const Type declared = resolveType(*assignment->declaredType);
            rejectSliceOutsideParameter(declared, "a local variable's declared type");
            if (!(declared == valueType))
            {
                throw std::runtime_error("variable '" + assignment->name + "' declared as " +
                                         typeName(declared) + " but initialized with " +
                                         typeName(valueType));
            }
        }
        env.define(assignment->name, valueType);
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt))
    {
        if (!expectedReturnType)
        {
            throw std::runtime_error("'return' used outside a function");
        }
        const Type valueType =
            returnStmt->value
                ? checkExpr(*returnStmt->value, env, expectedReturnType, currentLoopBreakTypes)
                : kUnit;
        if (!(valueType == *expectedReturnType))
        {
            throw std::runtime_error("'return' produces " + typeName(valueType) +
                                     " but function declares " + typeName(*expectedReturnType));
        }
        return;
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt))
    {
        checkExpr(*exprStmt->expr, env, expectedReturnType, currentLoopBreakTypes);
        return;
    }

    if (const auto* fieldAssign = dynamic_cast<const FieldAssignStmt*>(&stmt))
    {
        const Type fieldType = checkFieldType(*fieldAssign->object,
                                              fieldAssign->field,
                                              env,
                                              expectedReturnType,
                                              currentLoopBreakTypes);
        const Type valueType =
            checkExpr(*fieldAssign->value, env, expectedReturnType, currentLoopBreakTypes);
        if (!(fieldType == valueType))
        {
            throw std::runtime_error("field '" + fieldAssign->field + "' expects " +
                                     typeName(fieldType) + ", got " + typeName(valueType));
        }
        return;
    }

    if (const auto* indexAssign = dynamic_cast<const IndexAssignStmt*>(&stmt))
    {
        const Type objectType =
            checkExpr(*indexAssign->object, env, expectedReturnType, currentLoopBreakTypes);
        if (!isIndexable(objectType))
        {
            throw std::runtime_error("indexed assignment into non-array/slice type " +
                                     typeName(objectType));
        }
        const Type indexType =
            checkExpr(*indexAssign->index, env, expectedReturnType, currentLoopBreakTypes);
        if (!(indexType == kI32))
        {
            throw std::runtime_error("array index must be i32, found " + typeName(indexType));
        }
        if (objectType.kind == TypeKind::Array) // no static size to check a slice's index against
        {
            checkLiteralIndexBounds(*indexAssign->index, objectType);
        }

        const Type elementType = simpleType(objectType.elementKind, objectType.elementStructName);
        const Type valueType =
            checkExpr(*indexAssign->value, env, expectedReturnType, currentLoopBreakTypes);
        if (!(valueType == elementType))
        {
            throw std::runtime_error("array element expects " + typeName(elementType) + ", got " +
                                     typeName(valueType));
        }
        return;
    }

    if (const auto* incDec = dynamic_cast<const IncDecStmt*>(&stmt))
    {
        const Type targetType =
            checkExpr(*incDec->target, env, expectedReturnType, currentLoopBreakTypes);
        if (!(targetType == kI32))
        {
            throw std::runtime_error("'++'/'--' requires an i32 target, found " +
                                     typeName(targetType));
        }
        return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt))
    {
        const Type conditionType =
            checkExpr(*whileStmt->condition, env, expectedReturnType, currentLoopBreakTypes);
        if (!(conditionType == kBool))
        {
            throw std::runtime_error("while condition must be bool, found " +
                                     typeName(conditionType));
        }
        std::vector<Type>
            breakTypes; // fresh per loop - a break here can never target an outer loop
        checkBlock(
            static_cast<const BlockExpr&>(*whileStmt->body), env, expectedReturnType, &breakTypes);
        for (const Type& breakType : breakTypes)
        {
            if (!(breakType == kUnit))
            {
                throw std::runtime_error(
                    "'break' with a value is only allowed inside 'loop', not 'while'");
            }
        }
        return;
    }

    if (const auto* breakStmt = dynamic_cast<const BreakStmt*>(&stmt))
    {
        if (!currentLoopBreakTypes)
        {
            throw std::runtime_error("'break' used outside a loop");
        }
        const Type valueType =
            breakStmt->value
                ? checkExpr(*breakStmt->value, env, expectedReturnType, currentLoopBreakTypes)
                : kUnit;
        currentLoopBreakTypes->push_back(valueType);
        return;
    }

    if (dynamic_cast<const ContinueStmt*>(&stmt))
    {
        if (!currentLoopBreakTypes)
        {
            throw std::runtime_error("'continue' used outside a loop");
        }
        return;
    }

    throw std::runtime_error("unsupported statement");
}

Type TypeChecker::checkFieldType(const Expr& object,
                                 const std::string& field,
                                 TypeEnv& env,
                                 const Type* expectedReturnType,
                                 std::vector<Type>* currentLoopBreakTypes)
{
    const Type objectType = checkExpr(object, env, expectedReturnType, currentLoopBreakTypes);

    if (isIndexable(objectType))
    {
        if (field == "length")
        {
            return kI32;
        }
        throw std::runtime_error(typeName(objectType) + " has no field '" + field +
                                 "' (did you mean 'length'?)");
    }

    // Map<K,V>/Set<T> aren't indexable (unordered - no `[i]`), so this is a
    // standalone case rather than folded into isIndexable above (see
    // docs/language/0034-maps-and-sets.md).
    if (objectType.kind == TypeKind::Map || objectType.kind == TypeKind::Set)
    {
        if (field == "length")
        {
            return kI32;
        }
        throw std::runtime_error(typeName(objectType) + " has no field '" + field +
                                 "' (did you mean 'length'?)");
    }

    // SortedMap<K,V> isn't indexable either - no `[key]`/`[key] =` syntax
    // this phase, and no `for`-in iteration either (see
    // docs/language/0040-sorted-maps.md) - mirrors Map<K,V>/Set<T>'s own
    // identical restriction.
    if (objectType.kind == TypeKind::SortedMap)
    {
        if (field == "length")
        {
            return kI32;
        }
        throw std::runtime_error(typeName(objectType) + " has no field '" + field +
                                 "' (did you mean 'length'?)");
    }

    // SortedSet<T> isn't indexable either - same reasoning as SortedMap<K,V>
    // above (see docs/language/0041-sorted-sets.md).
    if (objectType.kind == TypeKind::SortedSet)
    {
        if (field == "length")
        {
            return kI32;
        }
        throw std::runtime_error(typeName(objectType) + " has no field '" + field +
                                 "' (did you mean 'length'?)");
    }

    // str isn't indexable either this phase - slicing (`s[..4]`) is its own
    // separate AST node, not FieldExpr (see docs/language/0045-str-slicing.md).
    // `.length`/`.bytes` (see docs/language/0047-unicode.md) - `.length`
    // counts Unicode codepoints, `.bytes` the raw byte count. Previously
    // str had no field access at all; both are new.
    if (objectType.kind == TypeKind::String)
    {
        if (field == "length" || field == "bytes")
        {
            return kI32;
        }
        throw std::runtime_error(typeName(objectType) + " has no field '" + field +
                                 "' (did you mean 'length' or 'bytes'?)");
    }

    // String isn't indexable either this phase - same reasoning as str
    // above (see docs/language/0042-string.md). `.length`/`.bytes` mirror
    // str's own identical split (see docs/language/0047-unicode.md) -
    // `.length` now counts codepoints, a real deliberate change from this
    // type's own original "byte count" framing; `.bytes` is the new name
    // for what `.length` used to mean.
    if (objectType.kind == TypeKind::OwnedString)
    {
        if (field == "length" || field == "bytes")
        {
            return kI32;
        }
        throw std::runtime_error(typeName(objectType) + " has no field '" + field +
                                 "' (did you mean 'length' or 'bytes'?)");
    }

    // Buffer isn't indexable either (see docs/language/0043-buffer.md).
    // "capacity" (bytes currently allocated) is the real, meaningful
    // distinction that makes Buffer's own amortized growth observable,
    // unlike every other collection here which reallocates every
    // push/append and so has no capacity worth exposing. Deliberately
    // spelled "length", not the design doc's own "len"
    // (docs/std/strings/0004-buffer.md) - kept consistent with every
    // other collection's own ".length" here rather than introducing the
    // one differently-spelled field in this codebase. "length"/"bytes"
    // split the same way str/String's own do (see
    // docs/language/0047-unicode.md).
    if (objectType.kind == TypeKind::Buffer)
    {
        if (field == "length" || field == "bytes" || field == "capacity")
        {
            return kI32;
        }
        throw std::runtime_error(typeName(objectType) + " has no field '" + field +
                                 "' (did you mean 'length', 'bytes', or 'capacity'?)");
    }

    // Stack<T> isn't indexable either - LIFO access only, via push/pop/peek,
    // no `[i]` (see docs/language/0035-stacks.md).
    if (objectType.kind == TypeKind::Stack)
    {
        if (field == "length")
        {
            return kI32;
        }
        throw std::runtime_error(typeName(objectType) + " has no field '" + field +
                                 "' (did you mean 'length'?)");
    }

    // LinkedList<T> isn't indexable either - node-based, front/back access
    // only, no `[i]` (see docs/language/0036-linked-lists.md).
    if (objectType.kind == TypeKind::LinkedList)
    {
        if (field == "length")
        {
            return kI32;
        }
        throw std::runtime_error(typeName(objectType) + " has no field '" + field +
                                 "' (did you mean 'length'?)");
    }

    // Queue<T> deliberately isn't indexable either, even though it's backed
    // by Deque<T>'s own (indexable) representation - "communicate intent"
    // (see docs/language/0029-collections.md's Guiding Principle and
    // docs/language/0038-queues.md), the same restriction Stack<T> already
    // accepts despite being LLVM-identical to indexable List<T>.
    if (objectType.kind == TypeKind::Queue)
    {
        if (field == "length")
        {
            return kI32;
        }
        throw std::runtime_error(typeName(objectType) + " has no field '" + field +
                                 "' (did you mean 'length'?)");
    }

    // PriorityQueue<T> isn't indexable either - a heap's internal array
    // order isn't even a meaningful order to expose, so "communicate
    // intent" (see docs/language/0038-queues.md's own identical framing for
    // Queue<T>, and docs/language/0039-priority-queues.md) applies here at
    // least as strongly.
    if (objectType.kind == TypeKind::PriorityQueue)
    {
        if (field == "length")
        {
            return kI32;
        }
        throw std::runtime_error(typeName(objectType) + " has no field '" + field +
                                 "' (did you mean 'length'?)");
    }

    if (objectType.kind != TypeKind::Struct)
    {
        throw std::runtime_error("field access on non-struct type " + typeName(objectType));
    }

    const StructDecl& decl = *structs_.at(objectType.structName);
    for (const auto& declaredField : decl.fields)
    {
        if (declaredField.name == field)
        {
            return resolveType(declaredField.type);
        }
    }
    throw std::runtime_error("struct '" + objectType.structName + "' has no field '" + field + "'");
}

Type TypeChecker::checkExpr(const Expr& expr,
                            TypeEnv& env,
                            const Type* expectedReturnType,
                            std::vector<Type>* currentLoopBreakTypes)
{
    if (dynamic_cast<const IntegerExpr*>(&expr))
    {
        return kI32;
    }

    if (dynamic_cast<const BoolExpr*>(&expr))
    {
        return kBool;
    }

    if (dynamic_cast<const StringExpr*>(&expr))
    {
        return simpleType(TypeKind::String);
    }

    if (dynamic_cast<const CharExpr*>(&expr))
    {
        return kChar;
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        return env.get(name->name);
    }

    if (const auto* block = dynamic_cast<const BlockExpr*>(&expr))
    {
        return checkBlock(*block, env, expectedReturnType, currentLoopBreakTypes);
    }

    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
    {
        const Type conditionType =
            checkExpr(*ifExpr->condition, env, expectedReturnType, currentLoopBreakTypes);
        if (!(conditionType == kBool))
        {
            throw std::runtime_error("if condition must be bool, found " + typeName(conditionType));
        }
        const Type thenType =
            checkExpr(*ifExpr->thenBranch, env, expectedReturnType, currentLoopBreakTypes);
        const Type elseType =
            checkExpr(*ifExpr->elseBranch, env, expectedReturnType, currentLoopBreakTypes);
        if (!(thenType == elseType))
        {
            throw std::runtime_error("if branches have incompatible types: then is " +
                                     typeName(thenType) + ", else is " + typeName(elseType));
        }
        return thenType;
    }

    if (const auto* loopExpr = dynamic_cast<const LoopExpr*>(&expr))
    {
        std::vector<Type> breakTypes; // fresh per loop - shadows any outer loop's collector
        checkBlock(
            static_cast<const BlockExpr&>(*loopExpr->body), env, expectedReturnType, &breakTypes);
        if (breakTypes.empty())
        {
            // A loop with no `break` anywhere never actually produces a
            // value (it's really `never`), but TypeKind::Never has no
            // checking logic wired up anywhere in this codebase yet - `unit`
            // is the pragmatic, harmless choice (documented imprecision, see
            // docs/language/0028-loops.md).
            return kUnit;
        }
        const Type& first = breakTypes.front();
        for (const Type& breakType : breakTypes)
        {
            if (!(breakType == first))
            {
                throw std::runtime_error(
                    "'break' values in the same loop must all have the same type, found " +
                    typeName(first) + " and " + typeName(breakType));
            }
        }
        return first;
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        const auto it = functions_.find(call->callee);
        if (it == functions_.end())
        {
            throw std::runtime_error("undefined function: " + call->callee);
        }

        const FunctionDecl& function = *it->second;
        if (call->arguments.size() != function.params.size())
        {
            throw std::runtime_error("function '" + call->callee + "' expects " +
                                     std::to_string(function.params.size()) + " argument(s), got " +
                                     std::to_string(call->arguments.size()));
        }

        for (std::size_t i = 0; i < call->arguments.size(); ++i)
        {
            const Type argType =
                checkExpr(*call->arguments[i], env, expectedReturnType, currentLoopBreakTypes);
            const Type paramType = resolveType(function.params[i].type);
            // An array of *any* size implicitly converts to a slice of the
            // same element type at a call boundary - the whole point of
            // slice<T> (docs/language/0032-slices.md). An existing slice
            // passed straight through to another slice parameter needs no
            // special-casing: ordinary Slice == Slice equality already
            // covers that forwarding case.
            const bool arrayToSliceCoercion =
                paramType.kind == TypeKind::Slice && argType.kind == TypeKind::Array &&
                argType.elementKind == paramType.elementKind &&
                argType.elementStructName == paramType.elementStructName;
            // An owned String implicitly lends a str at a call boundary -
            // the same "wider owned type stands in for a narrower borrowed
            // view" shape arrayToSliceCoercion already covers for arrays,
            // just for String/str instead (see docs/language/0042-string.md
            // and docs/std/strings/0001-str.md).
            const bool stringToStrCoercion =
                paramType == kStr && argType.kind == TypeKind::OwnedString;
            if (!(argType == paramType) && !arrayToSliceCoercion && !stringToStrCoercion)
            {
                throw std::runtime_error("argument " + std::to_string(i + 1) + " to '" +
                                         call->callee + "' expects " + typeName(paramType) +
                                         ", got " + typeName(argType));
            }
        }

        return function.returnType ? resolveType(*function.returnType) : kUnit;
    }

    if (const auto* methodCall = dynamic_cast<const MethodCallExpr*>(&expr))
    {
        const Type objectType =
            checkExpr(*methodCall->object, env, expectedReturnType, currentLoopBreakTypes);

        // `.parse<T>()` (see docs/language/0046-generic-methods.md) - the
        // first generic method call in this codebase, checked before the
        // object-type-keyed dispatch chain below since it applies across
        // both str-coercible TypeKinds (String == str, OwnedString ==
        // String) rather than being tied to one exact TypeKind the way
        // every other branch here is.
        if (methodCall->method == "parse")
        {
            if (!isStrCoercible(objectType))
            {
                throw std::runtime_error("'parse' requires str, got " + typeName(objectType));
            }
            if (methodCall->typeArgument.empty())
            {
                throw std::runtime_error(
                    "'parse' requires an explicit type argument, e.g. parse<i32>()");
            }
            if (!methodCall->arguments.empty())
            {
                throw std::runtime_error("'parse' expects 0 arguments, got " +
                                         std::to_string(methodCall->arguments.size()));
            }
            if (methodCall->typeArgument == "i32")
            {
                return kI32;
            }
            if (methodCall->typeArgument == "bool")
            {
                return kBool;
            }
            throw std::runtime_error("parse<" + methodCall->typeArgument +
                                     "> is not supported - only parse<i32> and parse<bool> are "
                                     "implemented this phase");
        }

        if (objectType.kind == TypeKind::List)
        {
            const Type elementType =
                simpleType(objectType.elementKind, objectType.elementStructName);

            if (methodCall->method == "push")
            {
                if (methodCall->arguments.size() != 1)
                {
                    throw std::runtime_error("'push' expects 1 argument, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type argType = checkExpr(
                    *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
                if (!(argType == elementType))
                {
                    throw std::runtime_error("'push' expects " + typeName(elementType) + ", got " +
                                             typeName(argType));
                }
                return kUnit;
            }

            if (methodCall->method == "pop")
            {
                if (!methodCall->arguments.empty())
                {
                    throw std::runtime_error("'pop' expects 0 arguments, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                return elementType;
            }

            throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                     typeName(objectType));
        }

        // Stack<T> (see docs/language/0035-stacks.md) - push/pop mirror
        // List<T>'s own byte-for-byte; peek is the one new operation, same
        // shape as pop (0 arguments, returns elementType) but - unlike pop -
        // doesn't remove (see RegionChecker for why that distinction
        // matters for struct-typed T).
        if (objectType.kind == TypeKind::Stack)
        {
            const Type elementType =
                simpleType(objectType.elementKind, objectType.elementStructName);

            if (methodCall->method == "push")
            {
                if (methodCall->arguments.size() != 1)
                {
                    throw std::runtime_error("'push' expects 1 argument, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type argType = checkExpr(
                    *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
                if (!(argType == elementType))
                {
                    throw std::runtime_error("'push' expects " + typeName(elementType) + ", got " +
                                             typeName(argType));
                }
                return kUnit;
            }

            if (methodCall->method == "pop" || methodCall->method == "peek")
            {
                if (!methodCall->arguments.empty())
                {
                    throw std::runtime_error("'" + methodCall->method +
                                             "' expects 0 arguments, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                return elementType;
            }

            throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                     typeName(objectType));
        }

        // LinkedList<T> (see docs/language/0036-linked-lists.md) - unlike
        // Stack<T>'s push/pop (which reuse List<T>'s own method names),
        // push_front/push_back/pop_front/pop_back are unique names nothing
        // else uses, so there's no ambiguity to resolve downstream (see
        // IrGenerator). No peek_front/peek_back this phase - every operation
        // either adds or removes, never aliases, so RegionChecker needs no
        // exception clause for LinkedList<T> at all.
        if (objectType.kind == TypeKind::LinkedList)
        {
            const Type elementType =
                simpleType(objectType.elementKind, objectType.elementStructName);

            if (methodCall->method == "push_front" || methodCall->method == "push_back")
            {
                if (methodCall->arguments.size() != 1)
                {
                    throw std::runtime_error("'" + methodCall->method +
                                             "' expects 1 argument, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type argType = checkExpr(
                    *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
                if (!(argType == elementType))
                {
                    throw std::runtime_error("'" + methodCall->method + "' expects " +
                                             typeName(elementType) + ", got " + typeName(argType));
                }
                return kUnit;
            }

            if (methodCall->method == "pop_front" || methodCall->method == "pop_back")
            {
                if (!methodCall->arguments.empty())
                {
                    throw std::runtime_error("'" + methodCall->method +
                                             "' expects 0 arguments, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                return elementType;
            }

            throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                     typeName(objectType));
        }

        // Deque<T> (see docs/language/0037-deques.md) - push_front/push_back/
        // pop_front/pop_back are the same method names LinkedList<T> uses
        // (IrGenerator resolves the ambiguity via isDequeExpr), but the type
        // checker itself already knows objectType.kind exactly, so there's
        // no ambiguity here at all. `[i]`/`[i]=`/`for`-in are handled
        // entirely by isIndexable's shared IndexExpr/IndexAssignStmt path -
        // no MethodCallExpr involvement, and no RegionChecker aliasing
        // exception either (pop_front/pop_back always remove).
        if (objectType.kind == TypeKind::Deque)
        {
            const Type elementType =
                simpleType(objectType.elementKind, objectType.elementStructName);

            if (methodCall->method == "push_front" || methodCall->method == "push_back")
            {
                if (methodCall->arguments.size() != 1)
                {
                    throw std::runtime_error("'" + methodCall->method +
                                             "' expects 1 argument, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type argType = checkExpr(
                    *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
                if (!(argType == elementType))
                {
                    throw std::runtime_error("'" + methodCall->method + "' expects " +
                                             typeName(elementType) + ", got " + typeName(argType));
                }
                return kUnit;
            }

            if (methodCall->method == "pop_front" || methodCall->method == "pop_back")
            {
                if (!methodCall->arguments.empty())
                {
                    throw std::runtime_error("'" + methodCall->method +
                                             "' expects 0 arguments, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                return elementType;
            }

            throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                     typeName(objectType));
        }

        // Queue<T> (see docs/language/0038-queues.md) - `enqueue`/`dequeue`
        // are brand-new method names nothing else in the language uses, so
        // (unlike Deque<T>'s own push_front/push_back/pop_front/pop_back)
        // there's no ambiguity anywhere, not even in IrGenerator. No `[i]`
        // (deliberately not indexable, "communicate intent" - see
        // isIndexable's own doc comment), and no RegionChecker aliasing
        // exception either (dequeue always removes).
        if (objectType.kind == TypeKind::Queue)
        {
            const Type elementType =
                simpleType(objectType.elementKind, objectType.elementStructName);

            if (methodCall->method == "enqueue")
            {
                if (methodCall->arguments.size() != 1)
                {
                    throw std::runtime_error("'enqueue' expects 1 argument, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type argType = checkExpr(
                    *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
                if (!(argType == elementType))
                {
                    throw std::runtime_error("'enqueue' expects " + typeName(elementType) +
                                             ", got " + typeName(argType));
                }
                return kUnit;
            }

            if (methodCall->method == "dequeue")
            {
                if (!methodCall->arguments.empty())
                {
                    throw std::runtime_error("'dequeue' expects 0 arguments, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                return elementType;
            }

            throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                     typeName(objectType));
        }

        // PriorityQueue<T> (see docs/language/0039-priority-queues.md) -
        // push/pop mirror Stack<T>'s own byte-for-byte; peek is Stack<T>.peek()'s
        // own twin (0 arguments, returns elementType, doesn't remove) - the
        // difference from Stack<T> is purely in *which* element push/pop/peek
        // touch (the heap's minimum, not the top of a LIFO stack), not in
        // their type-checking shape.
        if (objectType.kind == TypeKind::PriorityQueue)
        {
            const Type elementType =
                simpleType(objectType.elementKind, objectType.elementStructName);

            if (methodCall->method == "push")
            {
                if (methodCall->arguments.size() != 1)
                {
                    throw std::runtime_error("'push' expects 1 argument, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type argType = checkExpr(
                    *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
                if (!(argType == elementType))
                {
                    throw std::runtime_error("'push' expects " + typeName(elementType) + ", got " +
                                             typeName(argType));
                }
                return kUnit;
            }

            if (methodCall->method == "pop" || methodCall->method == "peek")
            {
                if (!methodCall->arguments.empty())
                {
                    throw std::runtime_error("'" + methodCall->method +
                                             "' expects 0 arguments, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                return elementType;
            }

            throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                     typeName(objectType));
        }

        // Map<K,V>/Set<T> (see docs/language/0034-maps-and-sets.md's generic
        // rewrite) - K/V are checked against their own resolved types
        // (elementTypeName/valueTypeName, re-resolved on demand), not a
        // hardcoded i32 the way this used to be fixed.
        if (objectType.kind == TypeKind::Map)
        {
            const Type keyType = resolveType(objectType.elementTypeName);
            const Type valueType = resolveType(objectType.valueTypeName);

            if (methodCall->method == "set")
            {
                if (methodCall->arguments.size() != 2)
                {
                    throw std::runtime_error("'set' expects 2 arguments, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type givenKeyType = checkExpr(
                    *methodCall->arguments[0], env, expectedReturnType, currentLoopBreakTypes);
                const Type givenValueType = checkExpr(
                    *methodCall->arguments[1], env, expectedReturnType, currentLoopBreakTypes);
                if (!(givenKeyType == keyType) || !(givenValueType == valueType))
                {
                    throw std::runtime_error("'set' expects (" + typeName(keyType) + ", " +
                                             typeName(valueType) + "), got (" +
                                             typeName(givenKeyType) + ", " +
                                             typeName(givenValueType) + ")");
                }
                return kUnit;
            }

            if (methodCall->method == "get" || methodCall->method == "contains" ||
                methodCall->method == "remove")
            {
                if (methodCall->arguments.size() != 1)
                {
                    throw std::runtime_error("'" + methodCall->method +
                                             "' expects 1 argument, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type givenKeyType = checkExpr(
                    *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
                if (!(givenKeyType == keyType))
                {
                    throw std::runtime_error("'" + methodCall->method + "' expects " +
                                             typeName(keyType) + ", got " + typeName(givenKeyType));
                }
                if (methodCall->method == "get")
                {
                    return valueType;
                }
                return methodCall->method == "contains" ? kBool : kUnit;
            }

            throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                     typeName(objectType));
        }

        if (objectType.kind == TypeKind::Set)
        {
            const Type elementType = resolveType(objectType.elementTypeName);

            if (methodCall->method == "add" || methodCall->method == "contains" ||
                methodCall->method == "remove")
            {
                if (methodCall->arguments.size() != 1)
                {
                    throw std::runtime_error("'" + methodCall->method +
                                             "' expects 1 argument, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type givenType = checkExpr(
                    *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
                if (!(givenType == elementType))
                {
                    throw std::runtime_error("'" + methodCall->method + "' expects " +
                                             typeName(elementType) + ", got " +
                                             typeName(givenType));
                }
                return methodCall->method == "contains" ? kBool : kUnit;
            }

            throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                     typeName(objectType));
        }

        // SortedMap<K,V> (see docs/language/0040-sorted-maps.md) - set/get/
        // contains/remove are byte-for-byte Map<K,V>'s own shape; the
        // difference (keeping keys ordered internally) is purely an
        // implementation detail below TypeChecker, not something that
        // changes any signature here.
        if (objectType.kind == TypeKind::SortedMap)
        {
            const Type keyType = resolveType(objectType.elementTypeName);
            const Type valueType = resolveType(objectType.valueTypeName);

            if (methodCall->method == "set")
            {
                if (methodCall->arguments.size() != 2)
                {
                    throw std::runtime_error("'set' expects 2 arguments, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type givenKeyType = checkExpr(
                    *methodCall->arguments[0], env, expectedReturnType, currentLoopBreakTypes);
                const Type givenValueType = checkExpr(
                    *methodCall->arguments[1], env, expectedReturnType, currentLoopBreakTypes);
                if (!(givenKeyType == keyType) || !(givenValueType == valueType))
                {
                    throw std::runtime_error("'set' expects (" + typeName(keyType) + ", " +
                                             typeName(valueType) + "), got (" +
                                             typeName(givenKeyType) + ", " +
                                             typeName(givenValueType) + ")");
                }
                return kUnit;
            }

            if (methodCall->method == "get" || methodCall->method == "contains" ||
                methodCall->method == "remove")
            {
                if (methodCall->arguments.size() != 1)
                {
                    throw std::runtime_error("'" + methodCall->method +
                                             "' expects 1 argument, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type givenKeyType = checkExpr(
                    *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
                if (!(givenKeyType == keyType))
                {
                    throw std::runtime_error("'" + methodCall->method + "' expects " +
                                             typeName(keyType) + ", got " + typeName(givenKeyType));
                }
                if (methodCall->method == "get")
                {
                    return valueType;
                }
                return methodCall->method == "contains" ? kBool : kUnit;
            }

            throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                     typeName(objectType));
        }

        // SortedSet<T> (see docs/language/0041-sorted-sets.md) - add/
        // contains/remove are byte-for-byte Set<T>'s own shape above; the
        // difference (keeping elements ordered internally) is purely an
        // implementation detail below TypeChecker.
        if (objectType.kind == TypeKind::SortedSet)
        {
            const Type elementType = resolveType(objectType.elementTypeName);

            if (methodCall->method == "add" || methodCall->method == "contains" ||
                methodCall->method == "remove")
            {
                if (methodCall->arguments.size() != 1)
                {
                    throw std::runtime_error("'" + methodCall->method +
                                             "' expects 1 argument, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type givenType = checkExpr(
                    *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
                if (!(givenType == elementType))
                {
                    throw std::runtime_error("'" + methodCall->method + "' expects " +
                                             typeName(elementType) + ", got " +
                                             typeName(givenType));
                }
                return methodCall->method == "contains" ? kBool : kUnit;
            }

            throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                     typeName(objectType));
        }

        // String (see docs/language/0042-string.md) - `append` accepts
        // anything str-coercible (a str, or another String - the same
        // "wider owned type lends the narrower borrowed view" rule
        // stringToStrCoercion above already applies at a plain call
        // boundary), and mutates in place, returning unit.
        if (objectType.kind == TypeKind::OwnedString)
        {
            if (methodCall->method == "append")
            {
                if (methodCall->arguments.size() != 1)
                {
                    throw std::runtime_error("'append' expects 1 argument, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type givenType = checkExpr(
                    *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
                if (!isStrCoercible(givenType))
                {
                    throw std::runtime_error("'append' expects str, got " + typeName(givenType));
                }
                return kUnit;
            }

            throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                     typeName(objectType));
        }

        // Buffer (see docs/language/0043-buffer.md) - `append`/
        // `append_line` accept anything str-coercible, same as
        // String.append() above (isStrCoercible is reused, not
        // reimplemented). `clear`/`reserve` mutate and return unit;
        // `reserve` additionally requires an i32 target capacity.
        // `finish` takes ownership of the buffer's own content and
        // returns it as a String.
        if (objectType.kind == TypeKind::Buffer)
        {
            if (methodCall->method == "append" || methodCall->method == "append_line")
            {
                if (methodCall->arguments.size() != 1)
                {
                    throw std::runtime_error("'" + methodCall->method +
                                             "' expects 1 argument, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type givenType = checkExpr(
                    *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
                if (!isStrCoercible(givenType))
                {
                    throw std::runtime_error("'" + methodCall->method + "' expects str, got " +
                                             typeName(givenType));
                }
                return kUnit;
            }

            if (methodCall->method == "clear")
            {
                if (!methodCall->arguments.empty())
                {
                    throw std::runtime_error("'clear' expects 0 arguments, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                return kUnit;
            }

            if (methodCall->method == "reserve")
            {
                if (methodCall->arguments.size() != 1)
                {
                    throw std::runtime_error("'reserve' expects 1 argument, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                const Type givenType = checkExpr(
                    *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
                if (!(givenType == kI32))
                {
                    throw std::runtime_error("'reserve' expects i32, got " + typeName(givenType));
                }
                return kUnit;
            }

            if (methodCall->method == "finish")
            {
                if (!methodCall->arguments.empty())
                {
                    throw std::runtime_error("'finish' expects 0 arguments, got " +
                                             std::to_string(methodCall->arguments.size()));
                }
                return simpleType(TypeKind::OwnedString);
            }

            throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                     typeName(objectType));
        }

        throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                 typeName(objectType));
    }

    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        return checkFieldType(
            *field->object, field->field, env, expectedReturnType, currentLoopBreakTypes);
    }

    if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expr))
    {
        const auto it = structs_.find(literal->typeName);
        if (it == structs_.end())
        {
            throw std::runtime_error("undefined struct type: " + literal->typeName);
        }

        const StructDecl& decl = *it->second;
        if (literal->fields.size() != decl.fields.size())
        {
            throw std::runtime_error("struct literal for '" + literal->typeName + "' has " +
                                     std::to_string(literal->fields.size()) +
                                     " field(s), expected " + std::to_string(decl.fields.size()));
        }

        for (const auto& declaredField : decl.fields)
        {
            const Expr* initializer = nullptr;
            for (const auto& [fieldName, fieldExpr] : literal->fields)
            {
                if (fieldName == declaredField.name)
                {
                    initializer = fieldExpr.get();
                    break;
                }
            }
            if (!initializer)
            {
                throw std::runtime_error("struct literal for '" + literal->typeName +
                                         "' is missing field '" + declaredField.name + "'");
            }

            const Type initType =
                checkExpr(*initializer, env, expectedReturnType, currentLoopBreakTypes);
            const Type declaredFieldType = resolveType(declaredField.type);
            if (!(initType == declaredFieldType))
            {
                throw std::runtime_error(
                    "field '" + declaredField.name + "' of '" + literal->typeName + "' expects " +
                    typeName(declaredFieldType) + ", got " + typeName(initType));
            }
        }

        return simpleType(TypeKind::Struct, literal->typeName);
    }

    if (const auto* arrayLiteral = dynamic_cast<const ArrayLiteralExpr*>(&expr))
    {
        if (arrayLiteral->elements.empty())
        {
            throw std::runtime_error(
                "cannot infer the element type of an empty array literal; add a type annotation");
        }

        const Type elementType = checkExpr(
            *arrayLiteral->elements.front(), env, expectedReturnType, currentLoopBreakTypes);
        for (std::size_t i = 1; i < arrayLiteral->elements.size(); ++i)
        {
            const Type thisType = checkExpr(
                *arrayLiteral->elements[i], env, expectedReturnType, currentLoopBreakTypes);
            if (!(thisType == elementType))
            {
                throw std::runtime_error(
                    "array literal elements must all have the same type, found " +
                    typeName(elementType) + " and " + typeName(thisType));
            }
        }

        return arrayLikeType(TypeKind::Array,
                             elementType.kind,
                             elementType.structName,
                             static_cast<int>(arrayLiteral->elements.size()));
    }

    if (const auto* listNew = dynamic_cast<const ListNewExpr*>(&expr))
    {
        const Type elementType = resolveType(listNew->elementType);
        if (elementType.kind == TypeKind::Array || elementType.kind == TypeKind::Slice ||
            elementType.kind == TypeKind::List || elementType.kind == TypeKind::Stack ||
            elementType.kind == TypeKind::LinkedList || elementType.kind == TypeKind::Deque ||
            elementType.kind == TypeKind::Queue || elementType.kind == TypeKind::PriorityQueue)
        {
            throw std::runtime_error(
                "nested array/slice/List/Stack/LinkedList/Deque/Queue/PriorityQueue element "
                "types are not supported: " +
                typeName(elementType));
        }
        return arrayLikeType(TypeKind::List, elementType.kind, elementType.structName);
    }

    if (const auto* stackNew = dynamic_cast<const StackNewExpr*>(&expr))
    {
        const Type elementType = resolveType(stackNew->elementType);
        if (elementType.kind == TypeKind::Array || elementType.kind == TypeKind::Slice ||
            elementType.kind == TypeKind::List || elementType.kind == TypeKind::Stack ||
            elementType.kind == TypeKind::LinkedList || elementType.kind == TypeKind::Deque ||
            elementType.kind == TypeKind::Queue || elementType.kind == TypeKind::PriorityQueue)
        {
            throw std::runtime_error(
                "nested array/slice/List/Stack/LinkedList/Deque/Queue/PriorityQueue element "
                "types are not supported: " +
                typeName(elementType));
        }
        return arrayLikeType(TypeKind::Stack, elementType.kind, elementType.structName);
    }

    if (const auto* linkedListNew = dynamic_cast<const LinkedListNewExpr*>(&expr))
    {
        const Type elementType = resolveType(linkedListNew->elementType);
        if (elementType.kind == TypeKind::Array || elementType.kind == TypeKind::Slice ||
            elementType.kind == TypeKind::List || elementType.kind == TypeKind::Stack ||
            elementType.kind == TypeKind::LinkedList || elementType.kind == TypeKind::Deque ||
            elementType.kind == TypeKind::Queue || elementType.kind == TypeKind::PriorityQueue)
        {
            throw std::runtime_error(
                "nested array/slice/List/Stack/LinkedList/Deque/Queue/PriorityQueue element "
                "types are not supported: " +
                typeName(elementType));
        }
        return arrayLikeType(TypeKind::LinkedList, elementType.kind, elementType.structName);
    }

    if (const auto* dequeNew = dynamic_cast<const DequeNewExpr*>(&expr))
    {
        const Type elementType = resolveType(dequeNew->elementType);
        if (elementType.kind == TypeKind::Array || elementType.kind == TypeKind::Slice ||
            elementType.kind == TypeKind::List || elementType.kind == TypeKind::Stack ||
            elementType.kind == TypeKind::LinkedList || elementType.kind == TypeKind::Deque ||
            elementType.kind == TypeKind::Queue || elementType.kind == TypeKind::PriorityQueue)
        {
            throw std::runtime_error(
                "nested array/slice/List/Stack/LinkedList/Deque/Queue/PriorityQueue element "
                "types are not supported: " +
                typeName(elementType));
        }
        return arrayLikeType(TypeKind::Deque, elementType.kind, elementType.structName);
    }

    if (const auto* queueNew = dynamic_cast<const QueueNewExpr*>(&expr))
    {
        const Type elementType = resolveType(queueNew->elementType);
        if (elementType.kind == TypeKind::Array || elementType.kind == TypeKind::Slice ||
            elementType.kind == TypeKind::List || elementType.kind == TypeKind::Stack ||
            elementType.kind == TypeKind::LinkedList || elementType.kind == TypeKind::Deque ||
            elementType.kind == TypeKind::Queue || elementType.kind == TypeKind::PriorityQueue)
        {
            throw std::runtime_error(
                "nested array/slice/List/Stack/LinkedList/Deque/Queue/PriorityQueue element "
                "types are not supported: " +
                typeName(elementType));
        }
        return arrayLikeType(TypeKind::Queue, elementType.kind, elementType.structName);
    }

    if (const auto* priorityQueueNew = dynamic_cast<const PriorityQueueNewExpr*>(&expr))
    {
        // Reuses resolveType's own i32-only enforcement for
        // "PriorityQueue<elem>" - same "delegate to resolveType for real
        // semantic validation" choice MapNewExpr/SetNewExpr make just below,
        // not the List/Stack/Deque/Queue's own copy-pasted structural check
        // (see docs/language/0039-priority-queues.md).
        return resolveType("PriorityQueue<" + priorityQueueNew->elementType + ">");
    }

    if (const auto* mapNew = dynamic_cast<const MapNewExpr*>(&expr))
    {
        // Reuses resolveType's own hashability enforcement for
        // "Map<key,value>" - constructing the exact same canonical string
        // form Parser::parseTypeName would, so the error (if any) is
        // identical regardless of whether Map<K,V> appears in type or
        // expression position (see docs/language/0034-maps-and-sets.md).
        return resolveType("Map<" + mapNew->keyType + "," + mapNew->valueType + ">");
    }

    if (const auto* setNew = dynamic_cast<const SetNewExpr*>(&expr))
    {
        return resolveType("Set<" + setNew->elementType + ">");
    }

    if (const auto* sortedSetNew = dynamic_cast<const SortedSetNewExpr*>(&expr))
    {
        // Reuses resolveType's own orderability enforcement for
        // "SortedSet<elem>" - same delegation PriorityQueueNewExpr/
        // SortedMapNewExpr make above (see docs/language/0041-sorted-sets.md).
        return resolveType("SortedSet<" + sortedSetNew->elementType + ">");
    }

    if (const auto* stringNew = dynamic_cast<const StringNewExpr*>(&expr))
    {
        // Unlike every collection above, `text` is a real sub-expression,
        // not a type name string - String isn't generic, so there's
        // nothing for resolveType's own delegation trick to reuse here
        // (see docs/language/0042-string.md). str-coercible (str or
        // another String) mirrors `.append`'s own identical requirement.
        const Type textType =
            checkExpr(*stringNew->text, env, expectedReturnType, currentLoopBreakTypes);
        if (!isStrCoercible(textType))
        {
            throw std::runtime_error("String(...) expects str, got " + typeName(textType));
        }
        return simpleType(TypeKind::OwnedString);
    }

    if (dynamic_cast<const BufferNewExpr*>(&expr))
    {
        return simpleType(TypeKind::Buffer);
    }

    if (const auto* sortedMapNew = dynamic_cast<const SortedMapNewExpr*>(&expr))
    {
        // Reuses resolveType's own orderability enforcement for
        // "SortedMap<key,value>" - same delegation MapNewExpr/SetNewExpr
        // make just above (see docs/language/0040-sorted-maps.md).
        return resolveType("SortedMap<" + sortedMapNew->keyType + "," + sortedMapNew->valueType +
                           ">");
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expr))
    {
        const Type objectType =
            checkExpr(*index->object, env, expectedReturnType, currentLoopBreakTypes);
        if (!isIndexable(objectType))
        {
            throw std::runtime_error("indexing into non-array/slice type " + typeName(objectType));
        }
        const Type indexType =
            checkExpr(*index->index, env, expectedReturnType, currentLoopBreakTypes);
        if (!(indexType == kI32))
        {
            throw std::runtime_error("array index must be i32, found " + typeName(indexType));
        }
        if (objectType.kind == TypeKind::Array) // no static size to check a slice's index against
        {
            checkLiteralIndexBounds(*index->index, objectType);
        }

        return simpleType(objectType.elementKind, objectType.elementStructName);
    }

    if (const auto* strSlice = dynamic_cast<const StrSliceExpr*>(&expr))
    {
        // Restricted to str-coercible objects (str or String) - a
        // deliberately narrower scope than IndexExpr's own array/slice
        // reach, and deliberately *not* extended to arrays/slices in this
        // phase (sub-slicing an array is explicitly out of scope per
        // docs/language/0032-slices.md's own "Explicitly out of scope"
        // section). See docs/language/0045-str-slicing.md.
        const Type objectType =
            checkExpr(*strSlice->object, env, expectedReturnType, currentLoopBreakTypes);
        if (!isStrCoercible(objectType))
        {
            throw std::runtime_error("slicing requires str, got " + typeName(objectType));
        }
        if (strSlice->start)
        {
            const Type startType =
                checkExpr(*strSlice->start, env, expectedReturnType, currentLoopBreakTypes);
            if (!(startType == kI32))
            {
                throw std::runtime_error("slice start must be i32, found " + typeName(startType));
            }
        }
        if (strSlice->end)
        {
            const Type endType =
                checkExpr(*strSlice->end, env, expectedReturnType, currentLoopBreakTypes);
            if (!(endType == kI32))
            {
                throw std::runtime_error("slice end must be i32, found " + typeName(endType));
            }
        }
        return kStr;
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr))
    {
        const Type leftType =
            checkExpr(*binary->left, env, expectedReturnType, currentLoopBreakTypes);
        const Type rightType =
            checkExpr(*binary->right, env, expectedReturnType, currentLoopBreakTypes);

        switch (binary->op)
        {
            case TokenKind::Plus:
            case TokenKind::Minus:
            case TokenKind::Star:
            case TokenKind::Slash: requireInt(leftType, rightType); return kI32;
            case TokenKind::Less:
            case TokenKind::LessEqual:
            case TokenKind::Greater:
            case TokenKind::GreaterEqual: requireOrdered(leftType, rightType); return kBool;
            case TokenKind::EqualEqual:
            case TokenKind::BangEqual:
                if (!(leftType == rightType))
                {
                    throw std::runtime_error("cannot compare " + typeName(leftType) + " and " +
                                             typeName(rightType));
                }
                return kBool;
            default: throw std::runtime_error("unsupported operator");
        }
    }

    throw std::runtime_error("unsupported expression");
}
