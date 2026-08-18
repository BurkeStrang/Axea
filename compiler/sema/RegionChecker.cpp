#include "sema/RegionChecker.hpp"

#include <stdexcept>

namespace
{
    bool isArrayTypeString(const std::string& type)
    {
        return !type.empty() && type.front() == '[';
    }

    // "[elem;N]" -> "elem" - the canonical, no-spaces form
    // Parser::parseTypeName always produces (see docs/language/0031-arrays.md).
    // Only used to decide whether an array's element type is itself
    // struct-typed, for IndexExpr's aliasing propagation.
    std::string arrayElementTypeName(const std::string& type)
    {
        return type.substr(1, type.find(';') - 1);
    }

    bool isListTypeString(const std::string& type)
    {
        return type.starts_with("List<");
    }

    // "List<elem>" -> "elem" - the canonical form Parser::parseTypeName
    // always produces (see docs/language/0033-lists.md). Mirrors
    // arrayElementTypeName above.
    std::string listElementTypeName(const std::string& type)
    {
        return type.substr(5, type.size() - 6);
    }

    bool isStackTypeString(const std::string& type)
    {
        return type.starts_with("Stack<");
    }

    // "Stack<elem>" -> "elem" - mirrors listElementTypeName above (see
    // docs/language/0035-stacks.md).
    std::string stackElementTypeName(const std::string& type)
    {
        return type.substr(6, type.size() - 7);
    }

    bool isLinkedListTypeString(const std::string& type)
    {
        return type.starts_with("LinkedList<");
    }

    bool isDequeTypeString(const std::string& type)
    {
        return type.starts_with("Deque<");
    }

    // "Deque<elem>" -> "elem" - mirrors listElementTypeName above (see
    // docs/language/0037-deques.md). Needed (unlike LinkedList's own
    // elementTypeName-less choice) because `[i]` on a struct-typed Deque
    // aliases the container - IndexExpr's already-generic aliasing case
    // reads this straight off objectInfo.elementStructType.
    std::string dequeElementTypeName(const std::string& type)
    {
        return type.substr(6, type.size() - 7);
    }

    // No queueElementTypeName - unlike Deque<T>, Queue<T> has no `[i]`/peek,
    // so there's no operation whose result could alias the container (see
    // docs/language/0038-queues.md). Mirrors LinkedList<T>'s own identical
    // choice for the identical reason.
    bool isQueueTypeString(const std::string& type)
    {
        return type.starts_with("Queue<");
    }

    // No priorityQueueElementTypeName either, for the identical reason -
    // PriorityQueue<T> has no `[i]`, and its `peek()` is only ever i32 this
    // phase (see docs/language/0039-priority-queues.md), so elementStructType
    // extraction would never have anything to populate anyway.
    bool isPriorityQueueTypeString(const std::string& type)
    {
        return type.starts_with("PriorityQueue<");
    }

    bool isMapTypeString(const std::string& type)
    {
        return type.starts_with("Map<");
    }

    bool isSetTypeString(const std::string& type)
    {
        return type.starts_with("Set<");
    }

    // No elementTypeName extraction needed, same reasoning as Set<T> above -
    // none of add/contains/remove ever return the stored element (see
    // docs/language/0041-sorted-sets.md).
    bool isSortedSetTypeString(const std::string& type)
    {
        return type.starts_with("SortedSet<");
    }

    // String isn't generic (see docs/language/0042-string.md), so this is
    // an exact match, not a starts_with prefix check. No elementTypeName
    // extraction needed either, same reasoning as Queue<T>/SortedSet<T>
    // above - `append` never returns the stored content, so there's no
    // aliasing case for MethodCallExpr to special-case.
    bool isStringTypeString(const std::string& type)
    {
        return type == "String";
    }

    // Buffer isn't generic either (see docs/language/0043-buffer.md), so
    // this is an exact match too. No elementTypeName extraction needed -
    // `finish()` returns a *fresh* String wrapping content the buffer no
    // longer owns after the call (its own header is reset to a fresh empty
    // buffer - see LlvmIrEmitter::emitBufferFinish), so it never aliases
    // the buffer's own post-call state; the ordinary "method call result is
    // Owned" default already covers it correctly, no exception needed.
    bool isBufferTypeString(const std::string& type)
    {
        return type == "Buffer";
    }

    // "Map<K,V>" -> "V". Own bracket-depth-aware top-level-comma split (per
    // this codebase's "each pass owns its own walk" convention - TypeChecker
    // has its own copy of this same logic, independently, for the same
    // reason: K/V can themselves be nested generics containing commas, e.g.
    // Map<i32,Map<i32,i32>>, since docs/language/0034-maps-and-sets.md's
    // generic rewrite). Only V is ever extracted here - RegionChecker only
    // needs this to propagate struct-aliasing through `.get()`'s result
    // (mirrors array/List's own elementStructType extraction); nothing ever
    // returns K itself, so K's own struct-ness is irrelevant here.
    std::string mapValueTypeName(const std::string& type)
    {
        const std::string args = type.substr(4, type.size() - 5); // strip "Map<" and trailing ">"
        int depth = 0;
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            if (args[i] == '<' || args[i] == '[')
            {
                ++depth;
            }
            else if (args[i] == '>' || args[i] == ']')
            {
                --depth;
            }
            else if (args[i] == ',' && depth == 0)
            {
                return args.substr(i + 1);
            }
        }
        return ""; // malformed - unreachable for a well-checked program
    }

    bool isSortedMapTypeString(const std::string& type)
    {
        return type.starts_with("SortedMap<");
    }

    // "SortedMap<K,V>" -> "V" - mirrors mapValueTypeName's own bracket-
    // depth-aware split above (see docs/language/0040-sorted-maps.md).
    // `.get()` is the only SortedMap operation that can hand back a value
    // aliasing the tree's own stored instance, same reasoning as Map<K,V>'s
    // own V extraction.
    std::string sortedMapValueTypeName(const std::string& type)
    {
        const std::string args =
            type.substr(10, type.size() - 11); // strip "SortedMap<" and trailing ">"
        int depth = 0;
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            if (args[i] == '<' || args[i] == '[')
            {
                ++depth;
            }
            else if (args[i] == '>' || args[i] == ']')
            {
                --depth;
            }
            else if (args[i] == ',' && depth == 0)
            {
                return args.substr(i + 1);
            }
        }
        return ""; // malformed - unreachable for a well-checked program
    }
} // namespace

RegionEnv::RegionEnv(const RegionEnv* parent)
    : parent_(parent)
{
}

void RegionEnv::define(const std::string& name, RegionInfo info)
{
    bindings_[name] = std::move(info);
}

RegionInfo RegionEnv::get(const std::string& name) const
{
    if (const auto it = bindings_.find(name); it != bindings_.end())
    {
        return it->second;
    }
    if (parent_)
    {
        return parent_->get(name);
    }
    throw std::runtime_error("undefined variable: " + name);
}

void RegionChecker::registerDecls(const Program& program)
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
}

void RegionChecker::requireOwned(const RegionInfo& info, const FunctionDecl& function) const
{
    if (info.kind == Region::Borrowed)
    {
        throw std::runtime_error(
            "function '" + function.name + "' cannot return '" + info.sourceParam +
            "': parameter '" + info.sourceParam +
            "' is borrowed and does not outlive the call - declare 'take' if ownership "
            "should transfer");
    }
}

void RegionChecker::checkFunction(const FunctionDecl& function,
                                  const std::vector<Capability>& capabilities)
{
    std::vector<Region> paramRegions;
    paramRegions.reserve(function.params.size());

    RegionEnv env;
    for (std::size_t i = 0; i < function.params.size(); ++i)
    {
        const auto& param = function.params[i];
        const std::string structType = structs_.contains(param.type) ? param.type : "";
        std::string elementStructType;
        const bool isArray = isArrayTypeString(param.type);
        const bool isList = isListTypeString(param.type);
        const bool isStack = isStackTypeString(param.type);
        const bool isMap = isMapTypeString(param.type);
        const bool isSet = isSetTypeString(param.type);
        // LinkedList<T> (see docs/language/0036-linked-lists.md) carries the
        // same aliasing risk as every other heap-allocated collection
        // (mustn't be treated as Owned unless `take`n) - but unlike
        // List<T>.pop()/Stack<T>.peek()/Map<K,V>.get(), no LinkedList<T>
        // operation this phase ever hands back a stored element without
        // removing it (no peek_front/peek_back), so there's no elementStructType
        // extraction to do here: MethodCallExpr's aliasing exception is never
        // consulted for LinkedList<T>.
        const bool isLinkedList = isLinkedListTypeString(param.type);
        const bool isDeque = isDequeTypeString(param.type);
        // Queue<T> (see docs/language/0038-queues.md) - same reasoning as
        // LinkedList<T> above: `dequeue()` always removes, and there's no
        // `[i]`/peek at all, so no elementStructType extraction is needed -
        // the simplest region-checking story of any collection this session.
        const bool isQueue = isQueueTypeString(param.type);
        // PriorityQueue<T> (see docs/language/0039-priority-queues.md) - same
        // reasoning as Queue<T> above: push/pop/peek's element is always i32
        // this phase, so no elementStructType extraction is ever needed.
        const bool isPriorityQueue = isPriorityQueueTypeString(param.type);
        const bool isSortedMap = isSortedMapTypeString(param.type);
        const bool isSortedSet = isSortedSetTypeString(param.type);
        const bool isString = isStringTypeString(param.type);
        const bool isBuffer = isBufferTypeString(param.type);
        if (isArray)
        {
            const std::string elementName = arrayElementTypeName(param.type);
            if (structs_.contains(elementName))
            {
                elementStructType = elementName;
            }
        }
        else if (isList)
        {
            const std::string elementName = listElementTypeName(param.type);
            if (structs_.contains(elementName))
            {
                elementStructType = elementName;
            }
        }
        else if (isStack)
        {
            // Same reasoning as List above - `.peek()` (unlike `.pop()`,
            // which removes) can hand back a value aliasing the stack's own
            // stored instance (see docs/language/0035-stacks.md).
            const std::string elementName = stackElementTypeName(param.type);
            if (structs_.contains(elementName))
            {
                elementStructType = elementName;
            }
        }
        else if (isMap)
        {
            // Only V (not K) - `.get()` is the only Map operation that can
            // hand back a value aliasing the map's own stored instance (see
            // docs/language/0034-maps-and-sets.md's generic rewrite); K is
            // never itself returned, so K's struct-ness doesn't matter here.
            // Set needs no equivalent: none of add/contains/remove ever
            // return the stored element.
            const std::string valueName = mapValueTypeName(param.type);
            if (structs_.contains(valueName))
            {
                elementStructType = valueName;
            }
        }
        else if (isDeque)
        {
            // Unlike LinkedList<T> above, Deque<T> genuinely needs this:
            // `[i]` on a struct-typed Deque aliases the container exactly
            // like List<T>[i] already does (see docs/language/0037-deques.md)
            // - IndexExpr's own regionOfExpr case is already fully generic,
            // so populating elementStructType here is the only wiring it
            // needs; no MethodCallExpr exception required (push_front/
            // push_back/pop_front/pop_back never alias).
            const std::string elementName = dequeElementTypeName(param.type);
            if (structs_.contains(elementName))
            {
                elementStructType = elementName;
            }
        }
        else if (isSortedMap)
        {
            // Same reasoning as Map above - `.get()` is the only SortedMap
            // operation that can hand back a value aliasing the tree's own
            // stored instance (see docs/language/0040-sorted-maps.md).
            const std::string valueName = sortedMapValueTypeName(param.type);
            if (structs_.contains(valueName))
            {
                elementStructType = valueName;
            }
        }
        // Struct-, array-, List-, Stack-, LinkedList-, Deque-, Queue-,
        // PriorityQueue-, Map-, Set-, SortedMap-, SortedSet-, String-, and
        // Buffer-typed parameters all carry aliasing risk (all are
        // heap-allocated, reference-semantics values - see
        // docs/language/0031-arrays.md, 0033-lists.md,
        // 0034-maps-and-sets.md, 0035-stacks.md, 0036-linked-lists.md,
        // 0037-deques.md, 0038-queues.md, 0039-priority-queues.md,
        // 0040-sorted-maps.md, 0041-sorted-sets.md, 0042-string.md, and
        // 0043-buffer.md); a primitive parameter is always Owned regardless
        // of its read/write/take capability.
        const bool borrowed = (!structType.empty() || isArray || isList || isStack ||
                               isLinkedList || isDeque || isQueue || isPriorityQueue || isMap ||
                               isSet || isSortedMap || isSortedSet || isString || isBuffer) &&
                              capabilities[i] != Capability::Take;
        env.define(param.name,
                   RegionInfo{borrowed ? Region::Borrowed : Region::Owned,
                              borrowed ? param.name : "",
                              structType,
                              elementStructType});
        paramRegions.push_back(borrowed ? Region::Borrowed : Region::Owned);
    }
    regions_[function.name] = std::move(paramRegions);

    // Nothing can leak through a non-struct, non-array, non-List, non-Stack,
    // non-LinkedList, non-Deque, non-Queue, non-PriorityQueue, non-Map,
    // non-Set, non-SortedMap, non-SortedSet, non-String, non-Buffer return
    // type: primitives are always copied by value, and unit carries no
    // value at all.
    if (!function.returnType ||
        (!structs_.contains(*function.returnType) && !isArrayTypeString(*function.returnType) &&
         !isListTypeString(*function.returnType) && !isStackTypeString(*function.returnType) &&
         !isLinkedListTypeString(*function.returnType) &&
         !isDequeTypeString(*function.returnType) && !isQueueTypeString(*function.returnType) &&
         !isPriorityQueueTypeString(*function.returnType) &&
         !isMapTypeString(*function.returnType) && !isSetTypeString(*function.returnType) &&
         !isSortedMapTypeString(*function.returnType) &&
         !isSortedSetTypeString(*function.returnType) &&
         !isStringTypeString(*function.returnType) && !isBufferTypeString(*function.returnType)))
    {
        return;
    }

    // Every individual `return` site is already checked independently,
    // however deeply nested in if/else (regionOfStmt's ReturnStmt case,
    // reached via this same walk) - functions require an explicit `return`
    // for anything but unit (docs/language/0023), so the body's own
    // top-level trailing value is no longer itself a return and must not be
    // wrapped in requireOwned. Still walk it for that recursive side effect.
    regionOfExpr(*function.body, env, function, nullptr);
}

RegionInfo RegionChecker::regionOfExpr(const Expr& expr,
                                       RegionEnv& env,
                                       const FunctionDecl& function,
                                       std::vector<RegionInfo>* currentLoopBreakRegions)
{
    if (dynamic_cast<const IntegerExpr*>(&expr) || dynamic_cast<const BoolExpr*>(&expr) ||
        dynamic_cast<const StringExpr*>(&expr) || dynamic_cast<const CharExpr*>(&expr))
    {
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        return env.get(name->name);
    }

    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        const RegionInfo objectInfo =
            regionOfExpr(*field->object, env, function, currentLoopBreakRegions);
        if (!objectInfo.structType.empty())
        {
            const auto it = structs_.find(objectInfo.structType);
            if (it != structs_.end())
            {
                for (const auto& declaredField : it->second->fields)
                {
                    if (declaredField.name != field->field)
                    {
                        continue;
                    }
                    if (structs_.contains(declaredField.type))
                    {
                        // Struct-typed field: aliases the same shared instance as the object.
                        return RegionInfo{
                            objectInfo.kind, objectInfo.sourceParam, declaredField.type};
                    }
                    // Primitive field: Value stores it by value, always a fresh copy.
                    return RegionInfo{Region::Owned, "", ""};
                }
            }
        }
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expr))
    {
        RegionInfo borrowed{Region::Owned, "", ""};
        bool anyBorrowed = false;
        for (const auto& [fieldName, fieldExpr] : literal->fields)
        {
            const RegionInfo fieldInfo =
                regionOfExpr(*fieldExpr, env, function, currentLoopBreakRegions);
            if (fieldInfo.kind == Region::Borrowed && !anyBorrowed)
            {
                anyBorrowed = true;
                borrowed = fieldInfo;
            }
        }
        if (anyBorrowed)
        {
            return RegionInfo{Region::Borrowed, borrowed.sourceParam, literal->typeName};
        }
        return RegionInfo{Region::Owned, "", literal->typeName};
    }

    if (const auto* arrayLiteral = dynamic_cast<const ArrayLiteralExpr*>(&expr))
    {
        RegionInfo borrowed{Region::Owned, "", ""};
        bool anyBorrowed = false;
        std::string elementStructType;
        for (const auto& element : arrayLiteral->elements)
        {
            const RegionInfo elementInfo =
                regionOfExpr(*element, env, function, currentLoopBreakRegions);
            if (!elementInfo.structType.empty())
            {
                elementStructType = elementInfo.structType;
            }
            if (elementInfo.kind == Region::Borrowed && !anyBorrowed)
            {
                anyBorrowed = true;
                borrowed = elementInfo;
            }
        }
        if (anyBorrowed)
        {
            return RegionInfo{Region::Borrowed, borrowed.sourceParam, "", elementStructType};
        }
        return RegionInfo{Region::Owned, "", "", elementStructType};
    }

    if (dynamic_cast<const ListNewExpr*>(&expr))
    {
        // A brand-new list starts empty - nothing to alias yet, always Owned
        // (see docs/language/0033-lists.md).
        return RegionInfo{Region::Owned, "", ""};
    }

    if (dynamic_cast<const MapNewExpr*>(&expr) || dynamic_cast<const SetNewExpr*>(&expr))
    {
        // Same reasoning as ListNewExpr above - a brand-new Map/Set starts
        // empty, always Owned (see docs/language/0034-maps-and-sets.md).
        return RegionInfo{Region::Owned, "", ""};
    }

    if (dynamic_cast<const SortedMapNewExpr*>(&expr))
    {
        // Same reasoning again - a brand-new tree starts empty, always Owned
        // (see docs/language/0040-sorted-maps.md).
        return RegionInfo{Region::Owned, "", ""};
    }

    if (dynamic_cast<const SortedSetNewExpr*>(&expr))
    {
        // Same reasoning again - a brand-new tree starts empty, always Owned
        // (see docs/language/0041-sorted-sets.md).
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* stringNew = dynamic_cast<const StringNewExpr*>(&expr))
    {
        // Same reasoning again - String(text) always allocates a fresh
        // buffer and copies text's own bytes into it, never aliasing
        // whatever text itself pointed at, so the result is always Owned
        // regardless of text's own region (see
        // docs/language/0042-string.md). Still walked (not skipped) for
        // the same recursive-region-tracking reason every other
        // sub-expression here is.
        regionOfExpr(*stringNew->text, env, function, currentLoopBreakRegions);
        return RegionInfo{Region::Owned, "", ""};
    }

    if (dynamic_cast<const BufferNewExpr*>(&expr))
    {
        // Same reasoning again - a brand-new buffer starts empty, always
        // Owned (see docs/language/0043-buffer.md).
        return RegionInfo{Region::Owned, "", ""};
    }

    if (dynamic_cast<const StackNewExpr*>(&expr))
    {
        // Same reasoning again - a brand-new stack starts empty, always
        // Owned (see docs/language/0035-stacks.md).
        return RegionInfo{Region::Owned, "", ""};
    }

    if (dynamic_cast<const LinkedListNewExpr*>(&expr))
    {
        // Same reasoning again - a brand-new linked list starts empty,
        // always Owned (see docs/language/0036-linked-lists.md).
        return RegionInfo{Region::Owned, "", ""};
    }

    if (dynamic_cast<const DequeNewExpr*>(&expr))
    {
        // Same reasoning again - a brand-new deque starts empty, always
        // Owned (see docs/language/0037-deques.md).
        return RegionInfo{Region::Owned, "", ""};
    }

    if (dynamic_cast<const QueueNewExpr*>(&expr))
    {
        // Same reasoning again - a brand-new queue starts empty, always
        // Owned (see docs/language/0038-queues.md).
        return RegionInfo{Region::Owned, "", ""};
    }

    if (dynamic_cast<const PriorityQueueNewExpr*>(&expr))
    {
        // Same reasoning again - a brand-new heap starts empty, always Owned
        // (see docs/language/0039-priority-queues.md).
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expr))
    {
        regionOfExpr(*index->index, env, function, currentLoopBreakRegions);
        const RegionInfo objectInfo =
            regionOfExpr(*index->object, env, function, currentLoopBreakRegions);
        if (!objectInfo.elementStructType.empty())
        {
            // Indexing into an array-of-structs aliases the same shared
            // instance as the array itself - mirrors FieldExpr's identical
            // rule for a struct-typed field.
            return RegionInfo{
                objectInfo.kind, objectInfo.sourceParam, objectInfo.elementStructType};
        }
        // Primitive-element array: indexing always yields a fresh copy.
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* strSlice = dynamic_cast<const StrSliceExpr*>(&expr))
    {
        // Same reasoning as StringNewExpr above - a slice always allocates
        // a fresh, independently-owned buffer and copies the relevant
        // bytes into it (see docs/language/0045-str-slicing.md), never
        // aliasing `object`'s own buffer, so the result is always Owned
        // regardless of `object`'s own region. All three sub-expressions
        // are still walked for the same recursive-region-tracking reason
        // every other sub-expression here is.
        regionOfExpr(*strSlice->object, env, function, currentLoopBreakRegions);
        if (strSlice->start)
        {
            regionOfExpr(*strSlice->start, env, function, currentLoopBreakRegions);
        }
        if (strSlice->end)
        {
            regionOfExpr(*strSlice->end, env, function, currentLoopBreakRegions);
        }
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        for (const auto& argument : call->arguments)
        {
            regionOfExpr(*argument, env, function, currentLoopBreakRegions);
        }

        std::string structType;
        const auto it = functions_.find(call->callee);
        if (it != functions_.end() && it->second->returnType &&
            structs_.contains(*it->second->returnType))
        {
            structType = *it->second->returnType;
        }
        // A call's result is always Owned: if the callee actually leaked a
        // borrow through its return, the callee's own check rejects it
        // independently - the caller never needs to re-verify it.
        return RegionInfo{Region::Owned, "", structType};
    }

    if (const auto* methodCall = dynamic_cast<const MethodCallExpr*>(&expr))
    {
        const RegionInfo objectInfo =
            regionOfExpr(*methodCall->object, env, function, currentLoopBreakRegions);
        for (const auto& argument : methodCall->arguments)
        {
            regionOfExpr(*argument, env, function, currentLoopBreakRegions);
        }
        // "push" returns unit. "pop" removes and returns an element - once
        // removed, nothing else in the list still aliases it, so it's always
        // Owned (never Borrowed), but if the list held structs, the popped
        // value's own structType still needs to propagate so a chained
        // `.field` on it resolves correctly (mirrors IndexExpr's identical
        // propagation for a struct-typed array/slice element).
        //
        // "get" (Map<K,V>, docs/language/0034-maps-and-sets.md) and "peek"
        // (Stack<T>, docs/language/0035-stacks.md) are the exceptions to
        // "Owned unless removed": unlike pop/remove, neither removes - the
        // container still holds the same instance afterward, so a
        // struct-typed element aliases the container exactly the way an
        // array/slice index read does. Reusing pop's "always Owned" rule
        // here would silently let a borrowed Map<K, StructV>/Stack<StructT>
        // parameter's stored struct escape a function's return.
        if ((methodCall->method == "get" || methodCall->method == "peek") &&
            !objectInfo.elementStructType.empty())
        {
            return RegionInfo{
                objectInfo.kind, objectInfo.sourceParam, objectInfo.elementStructType};
        }
        return RegionInfo{Region::Owned, "", objectInfo.elementStructType};
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr))
    {
        regionOfExpr(*binary->left, env, function, currentLoopBreakRegions);
        regionOfExpr(*binary->right, env, function, currentLoopBreakRegions);
        return RegionInfo{Region::Owned, "", ""}; // arithmetic/comparison always yields a primitive
    }

    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
    {
        regionOfExpr(*ifExpr->condition, env, function, currentLoopBreakRegions);
        const RegionInfo thenInfo =
            regionOfExpr(*ifExpr->thenBranch, env, function, currentLoopBreakRegions);
        const RegionInfo elseInfo =
            regionOfExpr(*ifExpr->elseBranch, env, function, currentLoopBreakRegions);
        if (thenInfo.kind == Region::Borrowed)
        {
            return thenInfo;
        }
        if (elseInfo.kind == Region::Borrowed)
        {
            return elseInfo;
        }
        return thenInfo;
    }

    if (const auto* loopExpr = dynamic_cast<const LoopExpr*>(&expr))
    {
        std::vector<RegionInfo> breakRegions; // fresh per loop - shadows any outer loop's collector
        regionOfExpr(*loopExpr->body, env, function, &breakRegions);
        // Mirrors IfExpr just above: conservatively Borrowed if *any*
        // reachable break could be - a single unsound break anywhere makes
        // the whole loop's contributed value unsound to return as-is.
        for (const RegionInfo& info : breakRegions)
        {
            if (info.kind == Region::Borrowed)
            {
                return info;
            }
        }
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* block = dynamic_cast<const BlockExpr*>(&expr))
    {
        RegionEnv blockEnv(&env);
        for (const auto& statement : block->statements)
        {
            regionOfStmt(*statement, blockEnv, function, currentLoopBreakRegions);
        }
        if (block->result)
        {
            return regionOfExpr(*block->result, blockEnv, function, currentLoopBreakRegions);
        }
        return RegionInfo{Region::Owned, "", ""};
    }

    return RegionInfo{Region::Owned, "", ""};
}

void RegionChecker::regionOfStmt(const Stmt& stmt,
                                 RegionEnv& env,
                                 const FunctionDecl& function,
                                 std::vector<RegionInfo>* currentLoopBreakRegions)
{
    if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(&stmt))
    {
        env.define(assignment->name,
                   regionOfExpr(*assignment->value, env, function, currentLoopBreakRegions));
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt))
    {
        if (returnStmt->value)
        {
            requireOwned(regionOfExpr(*returnStmt->value, env, function, currentLoopBreakRegions),
                         function);
        }
        return;
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt))
    {
        regionOfExpr(*exprStmt->expr, env, function, currentLoopBreakRegions);
        return;
    }

    if (const auto* fieldAssign = dynamic_cast<const FieldAssignStmt*>(&stmt))
    {
        regionOfExpr(*fieldAssign->object, env, function, currentLoopBreakRegions);
        regionOfExpr(*fieldAssign->value, env, function, currentLoopBreakRegions);
        return;
    }

    if (const auto* indexAssign = dynamic_cast<const IndexAssignStmt*>(&stmt))
    {
        regionOfExpr(*indexAssign->object, env, function, currentLoopBreakRegions);
        regionOfExpr(*indexAssign->index, env, function, currentLoopBreakRegions);
        regionOfExpr(*indexAssign->value, env, function, currentLoopBreakRegions);
        return;
    }

    if (const auto* incDec = dynamic_cast<const IncDecStmt*>(&stmt))
    {
        regionOfExpr(*incDec->target, env, function, currentLoopBreakRegions);
        return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt))
    {
        regionOfExpr(*whileStmt->condition, env, function, currentLoopBreakRegions);
        // `while` never produces a value (break-with-value is rejected by
        // TypeChecker already), so its own fresh collector's contents are
        // discarded here - only the recursive walk (for nested returns)
        // matters.
        std::vector<RegionInfo> discarded;
        regionOfExpr(*whileStmt->body, env, function, &discarded);
        return;
    }

    if (const auto* breakStmt = dynamic_cast<const BreakStmt*>(&stmt))
    {
        if (breakStmt->value && currentLoopBreakRegions)
        {
            currentLoopBreakRegions->push_back(
                regionOfExpr(*breakStmt->value, env, function, currentLoopBreakRegions));
        }
        return;
    }

    // ContinueStmt: nothing to do.
}

void RegionChecker::check(
    const Program& program,
    const std::unordered_map<std::string, std::vector<Capability>>& capabilities)
{
    registerDecls(program);

    for (const auto& item : program.items)
    {
        if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get()))
        {
            checkFunction(*function, capabilities.at(function->name));
        }
    }
}

const std::unordered_map<std::string, std::vector<Region>>& RegionChecker::regions() const
{
    return regions_;
}
