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
    : parent_(parent),
      movedAccumulator_(parent ? parent->movedAccumulator_ : nullptr)
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

bool RegionEnv::has(const std::string& name) const
{
    if (bindings_.contains(name))
    {
        return true;
    }
    return parent_ && parent_->has(name);
}

void RegionEnv::markMoved(const std::string& name)
{
    RegionInfo info = get(name);
    info.moved = true;
    define(name, info);
    if (movedAccumulator_)
    {
        movedAccumulator_->insert(name);
    }
}

void RegionEnv::setMovedAccumulator(std::unordered_set<std::string>* accumulator)
{
    movedAccumulator_ = accumulator;
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
        else if (const auto* enumDecl = dynamic_cast<const EnumDecl*>(item.get()))
        {
            enums_[enumDecl->name] = enumDecl;
        }
        else if (const auto* implDecl = dynamic_cast<const ImplDecl*>(item.get()))
        {
            // See docs/language/0062-display-trait.md - each impl method
            // is registered exactly like a top-level FunctionDecl.
            for (const auto& method : implDecl->methods)
            {
                functions_[method->name] = method.get();
            }
        }
        else if (const auto* externDecl = dynamic_cast<const ExternDecl*>(item.get());
                 externDecl && !externDecl->moduleName.empty())
        {
            moduleNames_.insert(externDecl->moduleName);
        }
    }

    // Modules (see docs/language/0066-modules.md) - see moduleNames_'s own comment.
    for (const auto& [name, function] : functions_)
    {
        if (const auto dot = name.rfind('.'); dot != std::string::npos)
        {
            moduleNames_.insert(name.substr(0, dot));
        }
    }
}

void RegionChecker::requireOwned(const Expr& sourceExpr,
                                 const RegionInfo& info,
                                 RegionEnv& env,
                                 const FunctionDecl& function) const
{
    if (info.kind == Region::Borrowed)
    {
        throw std::runtime_error(
            "function '" + function.name + "' cannot return '" + info.sourceParam +
            "': parameter '" + info.sourceParam +
            "' is borrowed and does not outlive the call - declare 'take' if ownership "
            "should transfer");
    }
    // Move semantics (structs/enums only): a returned Owned struct/enum value is consumed by the
    // return - mark it moved so the (unreachable, but still-checked-for-other-paths) code after a
    // `return` can't be mistaken for a further legal use of the same name.
    if (!info.structType.empty())
    {
        if (const auto* name = dynamic_cast<const NameExpr*>(&sourceExpr))
        {
            env.markMoved(name->name);
        }
    }
}

void RegionChecker::consumeOwned(const Expr& sourceExpr,
                                 const RegionInfo& info,
                                 RegionEnv& env,
                                 const FunctionDecl& function) const
{
    // Move-checking (this new mechanism) is scoped to structs/enums only, matching the
    // refcounting-removal work it replaces - every other heap type (array/List/Map/.../closures)
    // keeps only the pre-existing, narrower "safe to return" checking, unchanged.
    if (info.structType.empty())
    {
        return;
    }
    if (info.kind == Region::Borrowed)
    {
        std::string describedName = info.sourceParam;
        if (const auto* name = dynamic_cast<const NameExpr*>(&sourceExpr))
        {
            describedName = name->name;
        }
        throw std::runtime_error("function '" + function.name +
                                 "' cannot move out of borrowed value '" + describedName + "'");
    }
    if (const auto* name = dynamic_cast<const NameExpr*>(&sourceExpr))
    {
        env.markMoved(name->name);
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
        // Enums (see docs/language/0064-enums.md) carry the identical aliasing risk a struct
        // param does (both are heap-allocated, reference-semantics values reached via a raw
        // pointer) but were never added here when enums were introduced after this checker's own
        // original design - a real, separate gap found while wiring match-arm move-checking below:
        // an enum-typed param fell through every branch of the `borrowed` formula a few lines down
        // (not struct, not array, not any collection), making it *unconditionally* Region::Owned
        // regardless of its declared capability, silently defeating both the borrow-escape check
        // and (now) match-arm binding-region correctness for any borrowed enum param.
        // Shared<T> (the move-semantics work's own explicit refcounting escape hatch) - a real,
        // confirmed use-after-free found while testing: this checker previously had no concept
        // of "Shared<...>" at all, so a Shared<T> param fell through every branch of the
        // `borrowed` formula below exactly like the enum gap the comment above already
        // documents - unconditionally Region::Owned regardless of its declared capability. A
        // *borrowed* (no `take`) Shared<T> param was then genuinely dropped at the callee's own
        // function exit, freeing the object out from under the caller's own still-live handle.
        const bool isShared = param.type.starts_with("Shared<") && param.type.back() == '>';
        std::string structType =
            (structs_.contains(param.type) || enums_.contains(param.type)) ? param.type : "";
        if (isShared)
        {
            // Also drives consumeOwned's own move-tracking gate (non-empty structType), so a
            // Shared<T> value is move-tracked exactly like a plain struct/enum - see the
            // consumeOwned-based enforcement this checker already applies uniformly.
            structType = param.type;
        }
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
        // Closures (see the move-semantics work's closure drop-lifecycle addition) are heap-
        // allocated fat pointers exactly like Shared<T> above, and hit the identical gap: without
        // this, a closure param falls through every branch below and is unconditionally
        // Region::Owned regardless of its declared capability, so a *borrowed* (no `take`) closure
        // param would be genuinely dropped at the callee's own function exit - now that closures
        // have a real drop lifecycle, that frees the captures struct out from under the caller's
        // own still-live closure. structType intentionally stays untouched (not set to param.type)
        // - consumeOwned's own move-tracking stays scoped to structs/enums/Shared<T>, unchanged;
        // this only fixes the borrowed/owned classification itself.
        const bool isClosure = param.type.starts_with("fn(");
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
        const bool borrowed =
            (!structType.empty() || isArray || isList || isStack || isLinkedList || isDeque ||
             isQueue || isPriorityQueue || isMap || isSet || isSortedMap || isSortedSet ||
             isString || isBuffer || isShared || isClosure) &&
            capabilities[i] != Capability::Take;
        env.define(param.name,
                   RegionInfo{borrowed ? Region::Borrowed : Region::Owned,
                              borrowed ? param.name : "",
                              structType,
                              elementStructType});
        paramRegions.push_back(borrowed ? Region::Borrowed : Region::Owned);
    }
    regions_[function.name] = std::move(paramRegions);

    // NOTE: this used to early-return here when the function's own return type couldn't possibly
    // leak a struct/array/collection ("nothing can leak through a non-struct return type, skip the
    // whole body-walk"). That was sound for this checker's *original* job (return-safety only) but
    // is no longer sound now that this same walk also performs move-checking (see
    // consumeOwned/requireOwned's move-marking): a function returning `i32` can still contain an
    // illegal `Shape.Rect(p, p)` double-move with nothing to do with its own return type, and
    // skipping the walk would silently let that compile. The body is now always walked.

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
    if (dynamic_cast<const IntegerExpr*>(&expr) || dynamic_cast<const Int64Expr*>(&expr) ||
        dynamic_cast<const FloatExpr*>(&expr) || dynamic_cast<const BoolExpr*>(&expr) ||
        dynamic_cast<const StringExpr*>(&expr) || dynamic_cast<const CharExpr*>(&expr))
    {
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        // A bare top-level function name used where a closure-typed value is expected (see
        // docs/language/0067-closures.md's implicit function-reference-to-closure coercion,
        // and IrGenerator's own identical tryLowerFunctionRef special-case) - checked only when
        // it's *not* an actual local/param binding, so a real variable always wins first. A
        // function reference is a fresh value, never an alias of anything, so plain Owned with no
        // structType is correct - this was unreachable via a function's own body before (a
        // function reference used inside a function body was already covered some other way, or
        // this exact gap simply hadn't been exercised there), and only became reachable once
        // top-level statements started being move/region-checked at all.
        if (!env.has(name->name) && functions_.contains(name->name))
        {
            return RegionInfo{Region::Owned, "", ""};
        }
        // The actual move-checking enforcement point: every read of a name funnels through here,
        // so checking `moved` at this single site protects every downstream consuming use
        // uniformly (assignment source, call argument, struct-literal field, return value, ...) -
        // markMoved only ever *sets* the flag; nothing previously ever *checked* it, which is
        // exactly why `Shape.Rect(p, p)` wasn't actually being rejected (found via direct testing).
        const RegionInfo info = env.get(name->name);
        if (info.moved)
        {
            throw std::runtime_error("use of moved value '" + name->name + "'");
        }
        return info;
    }

    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        // `EnumName.Variant` (no parens - a no-payload variant, see
        // docs/language/0064-enums.md and MethodCallExpr's own identical check above).
        if (const auto* objectName = dynamic_cast<const NameExpr*>(field->object.get());
            objectName && enums_.contains(objectName->name))
        {
            // structType set to the enum's own name (not left empty) - a freshly constructed
            // no-payload variant is a real enum value move-checking needs to see, exactly like a
            // StructLiteralExpr's own typeName.
            return RegionInfo{Region::Owned, "", objectName->name};
        }

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
                    if (structs_.contains(declaredField.type) ||
                        enums_.contains(declaredField.type))
                    {
                        // Struct/enum-typed field: always Borrowed, regardless of the parent
                        // object's own region. Move semantics (this checker's own core rule) has
                        // no partial-move support - a field cannot be independently moved out of
                        // its parent while leaving the rest of the parent intact, so a field read
                        // is treated as a zero-cost borrow, scoped to (at most) the parent's own
                        // lifetime. `p.field` can still be read freely / passed to other borrowed
                        // params; only *moving* it (return, take-call argument, another owning
                        // field, a collection insert) is rejected, via consumeOwned's Borrowed
                        // check. (A whole-value match-arm destructure is the one supported way to
                        // move a payload out of something - there the *entire* scrutinee is
                        // consumed at once, so there's no partial-parent left to reason about.)
                        return RegionInfo{Region::Borrowed, field->field, declaredField.type};
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
        const auto structIt = structs_.find(literal->typeName);
        RegionInfo borrowed{Region::Owned, "", ""};
        bool anyBorrowed = false;
        for (const auto& [fieldName, fieldExpr] : literal->fields)
        {
            const RegionInfo fieldInfo =
                regionOfExpr(*fieldExpr, env, function, currentLoopBreakRegions);
            // Move semantics: a struct/enum-*owning* field consumes its value on construction -
            // case 03 (`Shape.Rect(p, p)`) fires here: the second field's own regionOfExpr call
            // (env.get on the NameExpr) sees `p` already marked moved by the first field's
            // consumeOwned call, just below, and throws "use of moved value" before we even get
            // back to this loop body.
            if (structIt != structs_.end())
            {
                for (const auto& declaredField : structIt->second->fields)
                {
                    if (declaredField.name == fieldName && (structs_.contains(declaredField.type) ||
                                                            enums_.contains(declaredField.type)))
                    {
                        consumeOwned(*fieldExpr, fieldInfo, env, function);
                        break;
                    }
                }
            }
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

    if (const auto* interpolated = dynamic_cast<const InterpolatedStringExpr*>(&expr))
    {
        // Same reasoning as StringNewExpr/StrSliceExpr above - building a
        // String from interpolation pieces always allocates a fresh
        // buffer (see docs/language/Axea_Printing_Formatting.md), never
        // aliasing any piece's own region.
        for (const auto& piece : interpolated->pieces)
        {
            if (piece.expr)
            {
                regionOfExpr(*piece.expr, env, function, currentLoopBreakRegions);
            }
        }
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        // Move semantics: a `take`-declared param consumes its argument. capabilities_ (not
        // regions_) is the lookup used here deliberately - regions_ is still being built
        // function-by-function in this same pass, so a forward reference to a callee defined
        // later in the file wouldn't have its regions_ entry populated yet; capabilities_ is a
        // complete, precomputed map handed to us whole by CapabilityChecker before this pass
        // even starts.
        const auto capIt = capabilities_.find(call->callee);
        for (std::size_t i = 0; i < call->arguments.size(); ++i)
        {
            const RegionInfo argInfo =
                regionOfExpr(*call->arguments[i], env, function, currentLoopBreakRegions);
            if (capIt != capabilities_.end() && i < capIt->second.size() &&
                capIt->second[i] == Capability::Take)
            {
                consumeOwned(*call->arguments[i], argInfo, env, function);
            }
        }

        std::string structType;
        const auto it = functions_.find(call->callee);
        if (it != functions_.end() && it->second->returnType &&
            (structs_.contains(*it->second->returnType) ||
             enums_.contains(*it->second->returnType)))
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
        // `EnumName.Variant(args)` construction (see docs/language/0064-enums.md) - checked
        // before recursing into `methodCall->object` as an ordinary value expression just
        // below, which would otherwise throw "undefined variable" trying to look up a bare
        // enum type name in `env`. Still walks each argument (a borrowed value wrapped in a
        // variant's own payload could otherwise silently escape undetected - same reasoning
        // SomeExpr/OkExpr/ErrExpr's own construction already established), always yields
        // Owned overall (a freshly-built value, not an alias of anything).
        if (const auto* objectName = dynamic_cast<const NameExpr*>(methodCall->object.get());
            objectName && enums_.contains(objectName->name))
        {
            const EnumDecl* enumDecl = enums_.at(objectName->name);
            const EnumVariant* variant = nullptr;
            for (const auto& candidate : enumDecl->variants)
            {
                if (candidate.name == methodCall->method)
                {
                    variant = &candidate;
                    break;
                }
            }
            for (std::size_t i = 0; i < methodCall->arguments.size(); ++i)
            {
                const RegionInfo argInfo =
                    regionOfExpr(*methodCall->arguments[i], env, function, currentLoopBreakRegions);
                // Move semantics: a struct/enum-typed payload field consumes its argument on
                // construction, mirroring StructLiteralExpr's own field-population check above -
                // `Shape.Rect(p, p)` is rejected the identical way, just through this path.
                if (variant && i < variant->fieldTypes.size() &&
                    (structs_.contains(variant->fieldTypes[i]) ||
                     enums_.contains(variant->fieldTypes[i])))
                {
                    consumeOwned(*methodCall->arguments[i], argInfo, env, function);
                }
            }
            // structType set to the enum's own name - see FieldExpr's identical no-payload case
            // above for why this matters (move-checking needs to see this is a real enum value).
            return RegionInfo{Region::Owned, "", objectName->name};
        }

        // `math.sqrt(x)` module-qualified call (see docs/language/0066-modules.md) - same
        // "bare name matching something known, checked before generic resolution" reasoning as
        // the enum check just above; the exact `env.get` bug this whole pattern exists to avoid
        // was first found (and documented) for enum's own EnumName.Variant construction, see
        // docs/language/0064-enums.md.
        if (const auto* moduleName = dynamic_cast<const NameExpr*>(methodCall->object.get());
            moduleName && moduleNames_.contains(moduleName->name))
        {
            std::string structType;
            if (const auto it = functions_.find(moduleName->name + "." + methodCall->method);
                it != functions_.end() && it->second->returnType)
            {
                structType = *it->second->returnType;
            }
            for (const auto& argument : methodCall->arguments)
            {
                regionOfExpr(*argument, env, function, currentLoopBreakRegions);
            }
            return RegionInfo{Region::Owned, "", structType};
        }

        const RegionInfo objectInfo =
            regionOfExpr(*methodCall->object, env, function, currentLoopBreakRegions);
        // Move semantics: this whole branch is builtin collection/string/buffer methods only -
        // real user-defined struct methods reach this checker as an ordinary CallExpr with a
        // mangled "Type.method" callee (receiver as arguments[0]), handled by the CallExpr case
        // above, not here. Inserting a struct/enum-typed value into a collection consumes it,
        // exactly like passing it to a `take` param - this is the fix for the collection-insert
        // UAF found this session (previously patched with a runtime retain; a compile-time move
        // error is strictly better). "set" is Map/SortedMap's own two-argument form (key, value) -
        // both can be struct/enum-typed and both get consumed on insert.
        static const std::unordered_set<std::string> insertingMethods = {
            "push", "push_front", "push_back", "enqueue", "add"};
        for (std::size_t i = 0; i < methodCall->arguments.size(); ++i)
        {
            const RegionInfo argInfo =
                regionOfExpr(*methodCall->arguments[i], env, function, currentLoopBreakRegions);
            if (insertingMethods.contains(methodCall->method) || methodCall->method == "set")
            {
                consumeOwned(*methodCall->arguments[i], argInfo, env, function);
            }
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

    if (const auto* cast = dynamic_cast<const CastExpr*>(&expr))
    {
        regionOfExpr(*cast->operand, env, function, currentLoopBreakRegions);
        return RegionInfo{Region::Owned, "", ""}; // a numeric cast always yields a primitive
    }

    if (const auto* shareExpr = dynamic_cast<const ShareExpr*>(&expr))
    {
        // `Shared(x)` moves x into the new allocation - a genuine consuming use, exactly like a
        // struct-literal field (TypeChecker's own ShareExpr case already guarantees x is
        // struct/enum-typed). The resulting Shared<T> value itself is deliberately *not*
        // move-tracked the way a plain struct/enum is (structType left empty below) - Shared<T>
        // carries its own real runtime refcount, so an untracked extra copy is at worst an extra
        // kept-alive reference (accepted, matching every other "leak, don't free" scope note in
        // this codebase), never a use-after-free the way it would be for a counter-free plain
        // struct.
        const RegionInfo valueInfo =
            regionOfExpr(*shareExpr->value, env, function, currentLoopBreakRegions);
        consumeOwned(*shareExpr->value, valueInfo, env, function);
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* someExpr = dynamic_cast<const SomeExpr*>(&expr))
    {
        // Still walked (not skipped) for the same recursive-region-tracking
        // reason CastExpr's own operand above is - a borrowed value wrapped
        // in Some(...) could otherwise silently escape undetected (see
        // docs/language/0052-optional.md). MVP payloads are always scalar
        // (from `.parse<T>()`), so this always yields Owned regardless of
        // the wrapped value's own region, same as CastExpr.
        regionOfExpr(*someExpr->value, env, function, currentLoopBreakRegions);
        return RegionInfo{Region::Owned, "", ""};
    }

    if (dynamic_cast<const NoneExpr*>(&expr))
    {
        return RegionInfo{Region::Owned, "", ""};
    }

    // Ok(value)/Err(value) (see docs/language/0063-result.md) - same
    // "still walked, always yields Owned" shape as SomeExpr above.
    if (const auto* okExpr = dynamic_cast<const OkExpr*>(&expr))
    {
        regionOfExpr(*okExpr->value, env, function, currentLoopBreakRegions);
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* errExpr = dynamic_cast<const ErrExpr*>(&expr))
    {
        regionOfExpr(*errExpr->value, env, function, currentLoopBreakRegions);
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* tryExpr = dynamic_cast<const TryExpr*>(&expr))
    {
        // `expr?` unwraps Optional<T>'s payload - propagates the operand's
        // own region exactly like IndexExpr's element-read does, rather
        // than defaulting to Owned, so a borrowed struct payload extracted
        // through `?` is still tracked correctly.
        return regionOfExpr(*tryExpr->operand, env, function, currentLoopBreakRegions);
    }

    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
    {
        regionOfExpr(*ifExpr->condition, env, function, currentLoopBreakRegions);

        // Move-tracking branch isolation: each branch gets its own child env (so a name moved on
        // one path never poisons the sibling path's own use of it) and its own fresh moved-name
        // accumulator, inherited automatically by every nested scope the branch creates (see
        // RegionEnv's constructor). After both branches are visited, a name moved in *either* one
        // is conservatively treated as moved for code after the whole if/else (no "only moved on
        // the taken path" precision - documented, safe-by-construction limitation).
        std::unordered_set<std::string> thenMoved;
        RegionEnv thenEnv(&env);
        thenEnv.setMovedAccumulator(&thenMoved);
        const RegionInfo thenInfo =
            regionOfExpr(*ifExpr->thenBranch, thenEnv, function, currentLoopBreakRegions);

        std::unordered_set<std::string> elseMoved;
        RegionEnv elseEnv(&env);
        elseEnv.setMovedAccumulator(&elseMoved);
        const RegionInfo elseInfo =
            regionOfExpr(*ifExpr->elseBranch, elseEnv, function, currentLoopBreakRegions);

        // Only merge names that actually exist in the outer scope - a name declared fresh
        // *inside* one branch (and possibly moved there too) has nothing to merge; env.get()
        // would throw "undefined variable" for it, or worse, coincidentally collide with an
        // unrelated same-named outer binding if we didn't filter here.
        for (const auto& name : thenMoved)
        {
            if (env.has(name))
            {
                env.markMoved(name);
            }
        }
        for (const auto& name : elseMoved)
        {
            if (env.has(name))
            {
                env.markMoved(name);
            }
        }

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

    // `match` (see docs/language/0064-enums.md) - same "conservatively Borrowed if any branch
    // could be" reasoning as IfExpr just above, generalized from two branches to N arms, with the
    // same branch-isolated move-tracking. Arm bindings' own Owned/Borrowed-ness now follows the
    // scrutinee's (see below), not unconditionally Owned as before - this is the fix for this
    // session's match-arm UAF (`Rect(a,b) => return a` on a borrowed scrutinee used to compile
    // with zero tracking; it's now the same borrow-escape error returning any other borrowed
    // value gets).
    if (const auto* matchExpr = dynamic_cast<const MatchExpr*>(&expr))
    {
        const RegionInfo scrutineeInfo =
            regionOfExpr(*matchExpr->scrutinee, env, function, currentLoopBreakRegions);

        // Move semantics: matching an Owned enum scrutinee consumes it exactly once, moving each
        // arm's payload bindings out - the scrutinee itself becomes unusable afterward (mirrors
        // any other consuming use). Matching a Borrowed scrutinee instead produces Borrowed
        // bindings, scoped to (at most) the scrutinee's own lifetime - the same "field access is a
        // zero-cost borrow, not a partial move" rule FieldExpr's struct/enum case uses, just for a
        // whole-value destructure instead of a single field. Either way this happens once, up
        // front, *not* per-arm - only one arm ever actually executes at runtime, but the
        // consumption itself doesn't depend on which one.
        if (!scrutineeInfo.structType.empty() && scrutineeInfo.kind == Region::Owned)
        {
            consumeOwned(*matchExpr->scrutinee, scrutineeInfo, env, function);
        }

        const EnumDecl* scrutineeEnum =
            !scrutineeInfo.structType.empty() && enums_.contains(scrutineeInfo.structType)
                ? enums_.at(scrutineeInfo.structType)
                : nullptr;

        RegionInfo result{Region::Owned, "", ""};
        bool haveResult = false;
        for (const auto& arm : matchExpr->arms)
        {
            const EnumVariant* variant = nullptr;
            if (scrutineeEnum)
            {
                for (const auto& candidate : scrutineeEnum->variants)
                {
                    if (candidate.name == arm.variantName)
                    {
                        variant = &candidate;
                        break;
                    }
                }
            }

            // Branch isolation (see IfExpr above for the full reasoning) - each arm gets its own
            // child env and fresh moved-name accumulator, merged into the outer env afterward.
            std::unordered_set<std::string> armMoved;
            RegionEnv armEnv(&env);
            armEnv.setMovedAccumulator(&armMoved);
            for (std::size_t i = 0; i < arm.bindingNames.size(); ++i)
            {
                std::string bindingStructType;
                if (variant && i < variant->fieldTypes.size() &&
                    (structs_.contains(variant->fieldTypes[i]) ||
                     enums_.contains(variant->fieldTypes[i])))
                {
                    bindingStructType = variant->fieldTypes[i];
                }
                // A primitive payload field (bindingStructType empty) is always Owned,
                // regardless of the scrutinee's own region - it's copied by value and can never
                // alias anything, the same "primitives don't alias" rule checkFunction's own
                // param-region computation already applies. Only a struct/enum-typed binding
                // actually inherits the scrutinee's Borrowed-ness.
                const Region bindingKind =
                    bindingStructType.empty() ? Region::Owned : scrutineeInfo.kind;
                armEnv.define(arm.bindingNames[i],
                              RegionInfo{bindingKind,
                                         bindingKind == Region::Borrowed ? arm.bindingNames[i] : "",
                                         bindingStructType});
            }
            const RegionInfo armInfo =
                regionOfExpr(*arm.body, armEnv, function, currentLoopBreakRegions);

            for (const auto& name : armMoved)
            {
                if (env.has(name))
                {
                    env.markMoved(name);
                }
            }

            if (armInfo.kind == Region::Borrowed)
            {
                return armInfo;
            }
            if (!haveResult)
            {
                result = armInfo;
                haveResult = true;
            }
        }
        return result;
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

    // A closure literal (see docs/language/0067-closures.md) - genuinely walked (not just
    // defaulted to Owned via the generic fallback below, which would silently skip checking the
    // closure's own body for region issues) via a real child RegionEnv whose own parent is the
    // *enclosing* scope, exactly like TypeChecker::checkExpr's own identical ClosureExpr case -
    // the whole point of "closing over" it. `currentLoopBreakRegions` is null for the closure's
    // own top-level body, mirroring TypeChecker's own identical choice there: a bare `break`/
    // `continue` with no loop of the closure's *own* is already rejected before RegionChecker
    // ever runs, so this can never actually be consulted unless the closure body has its own
    // nested loop, which sets its own fresh one when it's reached. The closure value itself is
    // always Owned - a freshly built value, never itself a reference back into anything.
    if (const auto* closureExpr = dynamic_cast<const ClosureExpr*>(&expr))
    {
        // Struct-typed closure parameters (see docs/language/0067-closures.md) - same
        // borrowed-iff-struct-typed-and-not-Take formula checkFunction uses for a real function's
        // own params (simplified to the struct-only case - a closure param's own
        // elementStructType-bearing collection types, e.g. List<Point>, are a narrower, still-
        // Owned-by-default imprecision this pass doesn't yet resolve for closures, matching this
        // checker's own pre-existing "closure literal is opaque" stance for anything but a bare
        // struct param). A closure not found in closureCapabilities_ (no struct-typed param to
        // begin with, or capabilities not threaded through by this particular caller) falls back
        // to every param being Owned - this checker's own original, always-safe behavior.
        const auto closureCapIt = closureCapabilities_.find(closureExpr);
        std::vector<Region> paramRegions;
        paramRegions.reserve(closureExpr->params.size());
        RegionEnv closureEnv(&env);
        for (std::size_t i = 0; i < closureExpr->params.size(); ++i)
        {
            const auto& param = closureExpr->params[i];
            const std::string structType =
                (structs_.contains(param.type) || enums_.contains(param.type)) ? param.type : "";
            const bool borrowed = !structType.empty() &&
                                  closureCapIt != closureCapabilities_.end() &&
                                  closureCapIt->second[i] != Capability::Take;
            const Region region = borrowed ? Region::Borrowed : Region::Owned;
            closureEnv.define(param.name,
                              RegionInfo{region, borrowed ? param.name : "", structType});
            paramRegions.push_back(region);
        }
        closureRegions_[closureExpr] = std::move(paramRegions);
        regionOfExpr(*closureExpr->body, closureEnv, function, nullptr);
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
        RegionInfo info = regionOfExpr(*assignment->value, env, function, currentLoopBreakRegions);
        // Move semantics: `x = p` (p an existing Owned struct/enum binding, or a Borrowed one -
        // which consumeOwned rejects) consumes the source. The *new* binding `x` starts out
        // unmoved regardless - it's a fresh, valid handle to whatever was just consumed (or, for
        // every non-struct/enum type, an ordinary no-op pass-through, unchanged from before).
        consumeOwned(*assignment->value, info, env, function);
        info.moved = false;
        env.define(assignment->name, info);
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt))
    {
        if (returnStmt->value)
        {
            const RegionInfo info =
                regionOfExpr(*returnStmt->value, env, function, currentLoopBreakRegions);
            requireOwned(*returnStmt->value, info, env, function);
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
        const RegionInfo objectInfo =
            regionOfExpr(*fieldAssign->object, env, function, currentLoopBreakRegions);
        const RegionInfo valueInfo =
            regionOfExpr(*fieldAssign->value, env, function, currentLoopBreakRegions);
        // Move semantics: `obj.field = value` re-populates an owning field, exactly like
        // StructLiteralExpr's own field population - the field-store/collection-insert escape gap
        // found this session (`self.field = borrowedParam` slipping past both checkers entirely).
        if (!objectInfo.structType.empty())
        {
            if (const auto structIt = structs_.find(objectInfo.structType);
                structIt != structs_.end())
            {
                for (const auto& declaredField : structIt->second->fields)
                {
                    if (declaredField.name == fieldAssign->field &&
                        (structs_.contains(declaredField.type) ||
                         enums_.contains(declaredField.type)))
                    {
                        consumeOwned(*fieldAssign->value, valueInfo, env, function);
                        break;
                    }
                }
            }
        }
        return;
    }

    if (const auto* indexAssign = dynamic_cast<const IndexAssignStmt*>(&stmt))
    {
        const RegionInfo objectInfo =
            regionOfExpr(*indexAssign->object, env, function, currentLoopBreakRegions);
        regionOfExpr(*indexAssign->index, env, function, currentLoopBreakRegions);
        const RegionInfo valueInfo =
            regionOfExpr(*indexAssign->value, env, function, currentLoopBreakRegions);
        // Move semantics: `arr[i] = value` on a struct/enum-element array/slice (the only
        // receivers IndexAssignStmt ever sees - List<T> uses .push()/.set(), handled above)
        // consumes value, mirroring the collection-insert fix above.
        if (!objectInfo.elementStructType.empty())
        {
            consumeOwned(*indexAssign->value, valueInfo, env, function);
        }
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
    const std::unordered_map<std::string, std::vector<Capability>>& capabilities,
    const std::unordered_map<const ClosureExpr*, std::vector<Capability>>& closureCapabilities)
{
    closureCapabilities_ = closureCapabilities;
    capabilities_ = capabilities;
    registerDecls(program);

    for (const auto& item : program.items)
    {
        if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get()))
        {
            checkFunction(*function, capabilities.at(function->name));
        }
        else if (const auto* implDecl = dynamic_cast<const ImplDecl*>(item.get()))
        {
            for (const auto& method : implDecl->methods)
            {
                checkFunction(*method, capabilities.at(method->name));
            }
        }
    }

    // Top-level executable statements (see docs/language/0020-compiler-architecture.md - the
    // implicit top-level "main") were never move/region-checked at all before this fix: this
    // loop only ever matched FunctionDecl/ImplDecl, silently skipping every other Stmt kind. This
    // is exactly the gap that let `u = User{...}; archive(u); print(u)` (a real SIGSEGV found
    // this session) compile clean - `archive`'s own `take` consumed `u` for real, but nothing
    // ever checked the top-level sequence for a use-after-move. One shared env and a synthetic,
    // paramless FunctionDecl (its `name` is only ever read for error-message text - see
    // requireOwned/consumeOwned - nothing else about it is dereferenced by regionOfExpr/
    // regionOfStmt) - top-level moves now persist across the whole program the same way a
    // function body's own moves persist across its own statements.
    static const FunctionDecl topLevelFunction("<top-level>", {}, std::nullopt, nullptr);
    RegionEnv topLevelEnv;
    std::vector<std::string> topLevelBindingNames;
    for (const auto& item : program.items)
    {
        if (dynamic_cast<const FunctionDecl*>(item.get()) ||
            dynamic_cast<const ImplDecl*>(item.get()) ||
            dynamic_cast<const StructDecl*>(item.get()) ||
            dynamic_cast<const EnumDecl*>(item.get()) ||
            dynamic_cast<const ExternDecl*>(item.get()))
        {
            continue;
        }
        if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(item.get()))
        {
            topLevelBindingNames.push_back(assignment->name);
        }
        regionOfStmt(*item, topLevelEnv, topLevelFunction, nullptr);
    }

    // See movedTopLevelBindings()'s own doc comment - IrGenerator's synthetic top-level auto-print
    // needs this, since it never goes through regionOfStmt/regionOfExpr at all (it isn't real AST).
    for (const auto& name : topLevelBindingNames)
    {
        if (topLevelEnv.get(name).moved)
        {
            movedTopLevelBindings_.insert(name);
        }
    }
}

const std::unordered_set<std::string>& RegionChecker::movedTopLevelBindings() const
{
    return movedTopLevelBindings_;
}

const std::unordered_map<std::string, std::vector<Region>>& RegionChecker::regions() const
{
    return regions_;
}

const std::unordered_map<const ClosureExpr*, std::vector<Region>>&
RegionChecker::closureRegions() const
{
    return closureRegions_;
}
