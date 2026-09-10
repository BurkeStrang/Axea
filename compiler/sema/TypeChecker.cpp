#include "sema/TypeChecker.hpp"

#include "sema/FormatSpec.hpp"

#include <algorithm>
#include <stdexcept>

namespace
{
    // Builds a Type with only `kind`/`structName` set, value-initializing
    // every other field (arraySize/elementTypeName/valueTypeName) - avoids
    // -Wmissing-field-initializers, which (unlike leaving every field
    // default via `Type{}`) does fire on any *partial* aggregate init, no
    // matter how many trailing fields are provided.
    Type simpleType(TypeKind kind, std::string structName = "")
    {
        Type type{};
        type.kind = kind;
        type.structName = std::move(structName);
        return type;
    }

    // Builds a Type for any single-type-parameter kind (Array/Slice/List/
    // Stack/LinkedList/Deque/Queue/PriorityQueue/Optional) -
    // `elementTypeName` must already be canonical (typeName(...) of the
    // resolved element, not raw source text - see Type::elementTypeName's
    // own comment for why this is a string, not a flat elementKind tag).
    Type arrayLikeType(TypeKind kind, std::string elementTypeName, int arraySize = 0)
    {
        Type type{};
        type.kind = kind;
        type.elementTypeName = std::move(elementTypeName);
        type.arraySize = arraySize;
        return type;
    }

    const Type kBool = simpleType(TypeKind::Bool);
    const Type kI32 = simpleType(TypeKind::I32);
    const Type kI64 = simpleType(TypeKind::I64);
    const Type kF64 = simpleType(TypeKind::F64);
    const Type kChar = simpleType(TypeKind::Char);
    const Type kUnit = simpleType(TypeKind::Unit);
    const Type kStr = simpleType(TypeKind::String);

    // True for the three TypeKinds with real arithmetic today: i32, i64
    // (both signed integers - same operations, just different width), and
    // f64 (real floating-point +/-/*//, unlike the integers' truncating
    // `/`). Every other declared numeric TypeKind (i8/i16/i128, every
    // unsigned width, f32) stays an "unsupported type" error via
    // resolveType, same as before this phase (see
    // docs/language/0005-type-system.md). Shared by requireInt below and
    // by CastExpr's own numeric-conversion check.
    bool isNumericKind(TypeKind kind)
    {
        return kind == TypeKind::I32 || kind == TypeKind::I64 || kind == TypeKind::F64;
    }

    // Arithmetic (`+`/`-`/`*`/`/`) - both operands must be the *same*
    // numeric kind (see isNumericKind above); `i32 + i64` is still
    // rejected, same as every other mixed-type binary op in this checker -
    // no implicit widening, convert explicitly first with `as` (see
    // docs/language/0005-type-system.md). Returns that shared kind as the
    // result type (not always kI32 - `i64 + i64` must yield `i64`).
    Type requireInt(const Type& left, const Type& right)
    {
        if (left.kind != right.kind || !isNumericKind(left.kind))
        {
            throw std::runtime_error(
                "arithmetic requires two i32, two i64, or two f64 operands, found " +
                typeName(left) + " and " + typeName(right));
        }
        return left;
    }

    // True for any TypeKind that has a real total order today: i32/i64
    // (numeric - see isNumericKind above; f64 too, via LLVM's *ordered*
    // fcmp predicates, so a NaN operand simply compares false against
    // everything, including itself, rather than being specially rejected -
    // not overengineered further this phase), char (codepoint order, see
    // docs/language/0044-char.md), and str (real lexicographic byte order,
    // via LlvmIrEmitter's own registerOrderRuntime/@axea.less.str - not
    // pointer identity). The *owned* String type is deliberately excluded
    // even though it's str-coercible everywhere else in this language -
    // ordering, like Set<T>/Map<K,V>'s own hashability below, only ever
    // considers the bare value type; convert to str first, the same way a
    // String already has to for any other str-only operation. Shared by
    // requireOrdered below and by PriorityQueue<T>/SortedMap<K,V>/
    // SortedSet<T>'s own element/key-type checks, which all mean the same
    // "orderable" (see docs/language/0039-priority-queues.md).
    bool isOrderableKind(TypeKind kind)
    {
        return isNumericKind(kind) || kind == TypeKind::Char || kind == TypeKind::String;
    }

    // Ordering (`<`/`<=`/`>`/`>=`) - unlike arithmetic (requireInt above,
    // i32-only), also accepts char and str (see isOrderableKind above). A
    // char is never itself accepted in requireInt - char + char, unlike
    // char < char, is deliberately still an error. Both sides must be the
    // *same* orderable kind - i32 < char is still rejected, same as every
    // other mixed-type binary op in this checker.
    void requireOrdered(const Type& left, const Type& right)
    {
        if (left.kind != right.kind || !isOrderableKind(left.kind))
        {
            throw std::runtime_error(
                "comparison requires two i32, two i64, two f64, two char, or two str operands, "
                "found " +
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

    // Types safe to cross an `extern c` boundary (see
    // docs/language/0048-ffi.md) - a real, deliberately narrow allowlist,
    // not "anything goes": `char` (i24) has no C equivalent width, and
    // every collection/String/Buffer is a heap object with an
    // Axea-specific memory layout no C caller could correctly interpret.
    // `str`/`cstr` are both bare, null-terminated i8* - representationally
    // identical, both allowed.
    bool isFfiSafeType(const Type& type)
    {
        // i64 and *T (see docs/language/0019-unsafe.md) widen this for malloc/free-shaped
        // externs (`extern c malloc(size: i64) -> *T`) - a raw pointer is exactly as FFI-safe as
        // cstr's own bare i8* (same representation, one level of indirection deeper), and a real
        // C `size_t` parameter needs 64 bits.
        return type == kI32 || type == kI64 || type == kBool || type == kStr ||
               type.kind == TypeKind::CStr || type.kind == TypeKind::Pointer;
    }

    // Types with a well-defined text representation this phase (see
    // docs/language/Axea_Printing_Formatting.md) - used by both string
    // interpolation (`"{expr}"`) and the `print`/`write` builtins. As of
    // docs/language/0054-collection-printing.md, this covers every
    // struct and collection kind too (registerStrbufRuntime/
    // emitStructToStringHelpers/registerCollectionToStrRuntime give each
    // one a real "stringify to a heap string" runtime function, not just
    // a "print directly" one - the distinction that blocked interpolation
    // specifically from supporting them before). `slice<T>` - a by-value
    // fat pointer (`{T*, i32}`), structurally unlike every other
    // collection's own heap-header convention - is included too as of
    // docs/language/0056-slice-printing.md: registerCollectionToStrRuntime
    // gained a dedicated by-value branch (extractvalue instead of
    // GEP+load), the one piece that was still missing. It remains
    // forbidden as a collection element type almost everywhere via
    // rejectSliceOutsideParameter, so this only ever fires for a
    // slice<T>-typed function parameter used directly, never a nested one.
    bool isTextRepresentable(const Type& type)
    {
        if (type == kI32 || type == kI64 || type == kF64 || type == kBool || type == kChar ||
            isStrCoercible(type))
        {
            return true;
        }
        switch (type.kind)
        {
            case TypeKind::Optional:
            case TypeKind::Result:
            case TypeKind::Enum:
            case TypeKind::Struct:
            case TypeKind::Array:
            case TypeKind::Slice:
            case TypeKind::LinkedList:
            case TypeKind::Map:
            case TypeKind::Set:
            case TypeKind::SortedMap:
            case TypeKind::SortedSet: return true;
            default: return false;
        }
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
        // List<T>/Deque<T> are deliberately NOT here (see docs/language/0006-generics.md's own
        // List<T>/Stack<T>/Deque<T> port follow-up) - both are real, user-declared generic
        // structs now (std/collections.ax), reached via `.get(i)`/`.set(i, v)` methods (general
        // struct dispatch), not `[]` sugar - and neither supports `for`-in anymore either.
        return type.kind == TypeKind::Array || type.kind == TypeKind::Slice;
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

    // List<T> is a real, user-declared generic struct now (see
    // docs/language/0006-generics.md's own List<T> port follow-up) - it may freely be a struct
    // field type too, like any other struct, so the restriction this phase's own intrinsic
    // implementation used to need no longer applies; no rejectListAsFieldType left here.

    // Same "kept restricted purely to bound this pass's surface area, not
    // for any deeper architectural reason" rationale slice<T>/List<T> used to share above -
    // see docs/language/0034-maps-and-sets.md.
    void rejectMapOrSetAsFieldType(const Type& type)
    {
        if (type.kind == TypeKind::Map || type.kind == TypeKind::Set)
        {
            throw std::runtime_error(typeName(type) +
                                     " is not supported as a struct field type in this phase");
        }
    }

    // Stack<T> is a real, user-declared generic struct now (see
    // docs/language/0006-generics.md's own List<T>/Stack<T> port follow-up) - it may freely be a
    // struct field type too, like any other struct; no rejectStackAsFieldType left here.

    // Same rationale again - see docs/language/0036-linked-lists.md.
    void rejectLinkedListAsFieldType(const Type& type)
    {
        if (type.kind == TypeKind::LinkedList)
        {
            throw std::runtime_error(
                "LinkedList<T> is not supported as a struct field type in this phase");
        }
    }

    // Deque<T> is a real, user-declared generic struct now (see
    // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T> port follow-up) - it may
    // freely be a struct field type too, like any other struct; no rejectDequeAsFieldType left
    // here.

    // Queue<T> is a real, user-declared generic struct now (see
    // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T> port follow-up) -
    // it may freely be a struct field type too, like any other struct; no
    // rejectQueueAsFieldType left here.

    // PriorityQueue<T> is a real, user-declared generic struct now (see
    // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T>/PriorityQueue<T>
    // port follow-up) - it may freely be a struct field type too, like any other struct; no
    // rejectPriorityQueueAsFieldType left here.

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
    // '('/')' tracked alongside '<'/'['/'>'/']' since docs/language/0067-closures.md's own
    // "fn(T1,T2)->R" closure type text can nest inside any other type's own argument list
    // (e.g. a Map<str, fn(i32)->i32> value type) exactly the way Map<K,V> itself already can.
    std::size_t findTopLevelComma(const std::string& text)
    {
        int depth = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '<' || text[i] == '[' || text[i] == '(')
            {
                ++depth;
            }
            else if (text[i] == '>' || text[i] == ']' || text[i] == ')')
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

    // Same reasoning as findTopLevelComma above, for `[elem;N]`'s own `;` -
    // needed as soon as `elem` can itself be a nested array (`[[i32;3];4]`),
    // whose own `;` would otherwise be found first by a naive
    // `text.find(';')` (see docs/language/0052-optional.md's own
    // follow-up). Only the semicolon needs this treatment, not the
    // trailing `]` - array-type text is always exactly one top-level
    // bracket pair, so the *last* `]` in the string is always the correct
    // outer one regardless of nesting, unlike `,` which can occur at any
    // depth in Map<K,V>'s own text.
    std::size_t findTopLevelSemicolon(const std::string& text)
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
            else if (text[i] == ';' && depth == 1)
            {
                return i;
            }
        }
        return std::string::npos;
    }

    // "fn(T1,T2)->R" -> ({T1,T2}, R) (see docs/language/0067-closures.md) - the closure-flavored
    // analogue of Result's own payload-splitting helper, used wherever a closure's own structured
    // signature (not just its canonical string identity) is actually needed: checking a closure
    // call's own argument count/types, and (by IrGenerator/Interpreter, each with their own copy)
    // building the closure's own runtime representation.
    std::pair<std::vector<std::string>, std::string>
    closureParamAndReturnTypes(const std::string& closureTypeName)
    {
        int depth = 0;
        std::size_t closeParen = std::string::npos;
        for (std::size_t i = 3; i < closureTypeName.size(); ++i) // "fn(" is always exactly 3 chars
        {
            const char c = closureTypeName[i];
            if (c == '<' || c == '[' || c == '(')
            {
                ++depth;
            }
            else if (c == '>' || c == ']' || c == ')')
            {
                if (depth == 0)
                {
                    closeParen = i;
                    break;
                }
                --depth;
            }
        }
        const std::string paramsCsv = closureTypeName.substr(3, closeParen - 3);
        const std::string returnType = closureTypeName.substr(closeParen + 3); // skip ")->"

        std::vector<std::string> params;
        if (!paramsCsv.empty())
        {
            std::size_t start = 0;
            while (true)
            {
                const auto comma = findTopLevelComma(paramsCsv.substr(start));
                if (comma == std::string::npos)
                {
                    params.push_back(paramsCsv.substr(start));
                    break;
                }
                params.push_back(paramsCsv.substr(start, comma));
                start += comma + 1;
            }
        }
        return {params, returnType};
    }
} // namespace

std::string typeName(const Type& type)
{
    switch (type.kind)
    {
        case TypeKind::Bool: return "bool";
        case TypeKind::I32: return "i32";
        case TypeKind::I64: return "i64";
        case TypeKind::F64: return "f64";
        case TypeKind::Char: return "char";
        case TypeKind::String: return "str";
        case TypeKind::Unit: return "unit";
        case TypeKind::Struct: return type.structName;
        case TypeKind::Enum: return type.structName;
        case TypeKind::Closure: return type.structName;
        // elementTypeName/valueTypeName are always already-canonical strings
        // (see Type::elementTypeName's own comment), so reconstruction is
        // trivial string concatenation - no nested Type to recurse into
        // here, for any of these (docs/language/0052-optional.md's own
        // follow-up moved Array/Slice/List/Stack/LinkedList/Deque/Queue/
        // PriorityQueue/Optional off their old flat elementKind tag onto
        // this exact same string-based representation Map/Set/SortedMap/
        // SortedSet already used, to support genuinely arbitrary nesting).
        case TypeKind::Array:
            return "[" + type.elementTypeName + "; " + std::to_string(type.arraySize) + "]";
        case TypeKind::Slice: return "slice<" + type.elementTypeName + ">";
        case TypeKind::Optional: return "Optional<" + type.elementTypeName + ">";
        case TypeKind::Shared: return "Shared<" + type.elementTypeName + ">";
        case TypeKind::Result:
            return "Result<" + type.elementTypeName + "," + type.valueTypeName + ">";
        case TypeKind::LinkedList: return "LinkedList<" + type.elementTypeName + ">";
        case TypeKind::Map: return "Map<" + type.elementTypeName + "," + type.valueTypeName + ">";
        case TypeKind::Set: return "Set<" + type.elementTypeName + ">";
        case TypeKind::SortedMap:
            return "SortedMap<" + type.elementTypeName + "," + type.valueTypeName + ">";
        case TypeKind::SortedSet: return "SortedSet<" + type.elementTypeName + ">";
        case TypeKind::OwnedString: return "String";
        case TypeKind::Buffer: return "Buffer";
        case TypeKind::CStr: return "cstr";
        case TypeKind::Pointer: return "*" + type.elementTypeName;
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

bool TypeEnv::contains(const std::string& name) const
{
    if (types_.contains(name))
    {
        return true;
    }
    return parent_ && parent_->contains(name);
}

const EnumDecl* TypeChecker::asEnumTypeName(const Expr& objectExpr) const
{
    const auto* name = dynamic_cast<const NameExpr*>(&objectExpr);
    if (!name)
    {
        return nullptr;
    }
    const auto it = enums_.find(name->name);
    return it != enums_.end() ? it->second : nullptr;
}

Type TypeChecker::resolveUnionType(const std::string& canonicalName) const
{
    if (!enums_.contains(canonicalName))
    {
        std::vector<EnumVariant> variants;
        std::size_t start = 0;
        while (true)
        {
            const auto bar = canonicalName.find('|', start);
            const std::string alternative = canonicalName.substr(
                start, bar == std::string::npos ? std::string::npos : bar - start);
            // Union alternatives are restricted to "simple" types - a
            // primitive, or a plain struct/enum name - because a match arm's
            // own variant pattern (e.g. `i32(n) => ...`) is always a single
            // Identifier token (see Parser::parseMatchExpr); "List<i32>" or
            // "[i32;3]" can never be spelled as one, so allowing them as an
            // alternative would make the resulting union's own variants
            // permanently unmatchable. Detected syntactically (any of these
            // characters means "not a single identifier"), not by resolving
            // and inspecting the TypeKind, since that would also have to
            // special-case String/Buffer's own bare-word-but-still-simple
            // shape for no benefit.
            if (alternative.find_first_of("<>[];,|") != std::string::npos)
            {
                throw std::runtime_error(
                    "union type alternative '" + alternative +
                    "' must be a simple type (a primitive, struct, or enum name) - "
                    "compound types (List<T>, Map<K,V>, arrays, nested unions, ...) aren't "
                    "supported as union alternatives");
            }
            resolveType(alternative); // throws if the alternative itself isn't a real type
            variants.push_back(EnumVariant{alternative, {alternative}});
            if (bar == std::string::npos)
            {
                break;
            }
            start = bar + 1;
        }
        auto decl = std::make_unique<EnumDecl>(canonicalName, std::move(variants));
        enums_[canonicalName] = decl.get();
        unionDecls_.push_back(std::move(decl));
    }
    return simpleType(TypeKind::Enum, canonicalName);
}

bool TypeChecker::isUnionMember(const Type& valueType, const Type& targetType) const
{
    if (targetType.kind != TypeKind::Enum || targetType.structName.find('|') == std::string::npos)
    {
        return false;
    }
    const auto it = enums_.find(targetType.structName);
    if (it == enums_.end())
    {
        return false;
    }
    const std::string valueTypeName = typeName(valueType);
    for (const EnumVariant& variant : it->second->variants)
    {
        if (variant.fieldTypes.size() == 1 && variant.fieldTypes.front() == valueTypeName)
        {
            return true;
        }
    }
    return false;
}

bool TypeChecker::isFunctionRefAssignableToClosure(const Expr& expr,
                                                   const Type& targetType,
                                                   TypeEnv& env) const
{
    if (targetType.kind != TypeKind::Closure)
    {
        return false;
    }
    const auto* name = dynamic_cast<const NameExpr*>(&expr);
    if (!name || env.contains(name->name))
    {
        return false;
    }
    const auto it = functions_.find(name->name);
    if (it == functions_.end())
    {
        return false;
    }

    const auto [paramTypeNames, returnTypeName] = closureParamAndReturnTypes(targetType.structName);
    if (it->second->params.size() != paramTypeNames.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < paramTypeNames.size(); ++i)
    {
        if (typeName(resolveType(it->second->params[i].type)) != paramTypeNames[i])
        {
            return false;
        }
    }
    const std::string actualReturn =
        it->second->returnType ? typeName(resolveType(*it->second->returnType)) : "unit";
    return actualReturn == returnTypeName;
}

Type TypeChecker::closureSignatureType(const ClosureExpr& closureExpr) const
{
    std::string paramsCsv;
    for (std::size_t i = 0; i < closureExpr.params.size(); ++i)
    {
        if (i > 0)
        {
            paramsCsv += ",";
        }
        paramsCsv += typeName(resolveType(closureExpr.params[i].type));
    }
    const Type returnType = closureExpr.returnType ? resolveType(*closureExpr.returnType) : kUnit;
    Type result{};
    result.kind = TypeKind::Closure;
    result.structName = "fn(" + paramsCsv + ")->" + typeName(returnType);
    return result;
}

Type TypeChecker::checkCallArguments(const std::string& calleeDisplayName,
                                     const std::vector<Param>& params,
                                     const std::optional<std::string>& returnType,
                                     const std::vector<std::unique_ptr<Expr>>& arguments,
                                     TypeEnv& env,
                                     const Type* expectedReturnType,
                                     std::vector<Type>* currentLoopBreakTypes)
{
    if (arguments.size() != params.size())
    {
        throw std::runtime_error("function '" + calleeDisplayName + "' expects " +
                                 std::to_string(params.size()) + " argument(s), got " +
                                 std::to_string(arguments.size()));
    }

    for (std::size_t i = 0; i < arguments.size(); ++i)
    {
        const Type paramType = resolveType(params[i].type);

        // A bare top-level function name, used where a closure type is expected (see
        // docs/language/0067-closures.md) - checked before the generic checkExpr call just
        // below, which would otherwise throw "undefined variable" trying to resolve a bare
        // function name as if it were bound to a value.
        if (isFunctionRefAssignableToClosure(*arguments[i], paramType, env))
        {
            continue;
        }

        const Type argType =
            checkExpr(*arguments[i], env, expectedReturnType, currentLoopBreakTypes);
        // An array of *any* size implicitly converts to a slice of the
        // same element type at a call boundary - the whole point of
        // slice<T> (docs/language/0032-slices.md). An existing slice
        // passed straight through to another slice parameter needs no
        // special-casing: ordinary Slice == Slice equality already
        // covers that forwarding case.
        const bool arrayToSliceCoercion = paramType.kind == TypeKind::Slice &&
                                          argType.kind == TypeKind::Array &&
                                          argType.elementTypeName == paramType.elementTypeName;
        // An owned String implicitly lends a str at a call boundary -
        // the same "wider owned type stands in for a narrower borrowed
        // view" shape arrayToSliceCoercion already covers for arrays,
        // just for String/str instead (see docs/language/0042-string.md
        // and docs/std/strings/0001-str.md).
        const bool stringToStrCoercion = paramType == kStr && argType.kind == TypeKind::OwnedString;
        // Implicit union wrapping (see docs/language/0065-unions.md) -
        // `f(5)`/`f("hi")` against `f(x: i32 | str)` need no wrapper
        // syntax, the actual ergonomic point of the feature.
        const bool unionWrapCoercion = isUnionMember(argType, paramType);
        if (!(argType == paramType) && !arrayToSliceCoercion && !stringToStrCoercion &&
            !unionWrapCoercion)
        {
            throw std::runtime_error("argument " + std::to_string(i + 1) + " to '" +
                                     calleeDisplayName + "' expects " + typeName(paramType) +
                                     ", got " + typeName(argType));
        }
    }

    return returnType ? resolveType(*returnType) : kUnit;
}

Type TypeChecker::resolveType(const std::string& name) const
{
    static const std::unordered_map<std::string, TypeKind> primitives{
        {"bool", TypeKind::Bool},
        {"i32", TypeKind::I32},
        {"i64", TypeKind::I64},
        {"f64", TypeKind::F64},
        {"char", TypeKind::Char},
        {"cstr", TypeKind::CStr},
        {"str", TypeKind::String},
        {"unit", TypeKind::Unit},
    };

    // "*T" - the canonical form Parser::parseTypeNameAtom always produces (see
    // docs/language/0019-unsafe.md). A full recursive resolveType() call, not a single token, so
    // "**T" resolves for free via plain recursion, mirroring every other single-type-parameter
    // kind's own "store the canonical resolved string" convention (see arrayLikeType below).
    if (!name.empty() && name.front() == '*')
    {
        const Type pointeeType = resolveType(name.substr(1));
        return arrayLikeType(TypeKind::Pointer, typeName(pointeeType));
    }

    // "[elem;N]" - the canonical (no-spaces) form Parser::parseTypeName
    // always produces (see docs/language/0031-arrays.md).
    if (!name.empty() && name.front() == '[')
    {
        const auto semicolon = findTopLevelSemicolon(name);
        const auto closeBracket = name.rfind(']');
        if (semicolon == std::string::npos || closeBracket == std::string::npos)
        {
            throw std::runtime_error("malformed array type: " + name);
        }

        const std::string elementName = name.substr(1, semicolon - 1);
        const std::string sizeText = name.substr(semicolon + 1, closeBracket - semicolon - 1);
        const Type elementType = resolveType(elementName);

        return arrayLikeType(TypeKind::Array, typeName(elementType), std::stoi(sizeText));
    }

    // "slice<elem>" - the canonical form Parser::parseTypeName always
    // produces (see docs/language/0032-slices.md).
    if (name.starts_with("slice<") && name.back() == '>')
    {
        const std::string elementName = name.substr(6, name.size() - 7);
        const Type elementType = resolveType(elementName);

        return arrayLikeType(TypeKind::Slice, typeName(elementType));
    }

    // "Optional<elem>" - the canonical form Parser::parseTypeName always
    // produces (see docs/language/0052-optional.md).
    if (name.starts_with("Optional<") && name.back() == '>')
    {
        const std::string elementName = name.substr(9, name.size() - 10);
        const Type elementType = resolveType(elementName);

        return arrayLikeType(TypeKind::Optional, typeName(elementType));
    }

    // "Shared<elem>" - the canonical form Parser::parseTypeName always produces (see the
    // move-semantics RFC). No struct/enum restriction enforced here (resolveType is a pure
    // syntactic resolver, reused everywhere a type string appears) - that check lives in
    // ShareExpr's own checkExpr case, the one place a Shared<T> value can actually be constructed.
    if (name.starts_with("Shared<") && name.back() == '>')
    {
        const std::string elementName = name.substr(7, name.size() - 8);
        const Type elementType = resolveType(elementName);

        return arrayLikeType(TypeKind::Shared, typeName(elementType));
    }

    // "Result<T,E>" - the canonical form Parser::parseTypeName always
    // produces (see docs/language/0063-result.md). Same two-type-argument
    // shape as Map<K,V> below (bracket-depth-aware comma split via
    // findTopLevelComma, so Result<i32,Map<i32,i32>> splits correctly),
    // but with no isHashable/isOrderableKind gate on either T or E -
    // unlike Map's key, neither is ever hashed or compared, only stored
    // and propagated (mirrors V's own lack of restriction there).
    if (name.starts_with("Result<") && name.back() == '>')
    {
        const std::string args = name.substr(7, name.size() - 8);
        const auto comma = findTopLevelComma(args);
        if (comma == std::string::npos)
        {
            throw std::runtime_error("malformed Result type: " + name);
        }
        const std::string okName = args.substr(0, comma);
        const std::string errName = args.substr(comma + 1);
        const Type okType = resolveType(okName);
        const Type errType = resolveType(errName);
        rejectSliceOutsideParameter(okType, "a Result Ok type");
        rejectSliceOutsideParameter(errType, "a Result Err type");
        Type result{};
        result.kind = TypeKind::Result;
        result.elementTypeName = typeName(okType);
        result.valueTypeName = typeName(errType);
        return result;
    }

    // "List<elem>" is deliberately NOT recognized here anymore (see
    // docs/language/0006-generics.md's own List<T> port follow-up) - it's a real, user-declared
    // generic struct now (std/collections.ax); by the time this runs, GenericMonomorphizer has
    // already rewritten any concrete "List<i32>" reference to its own mangled struct name
    // ("List$i32"), resolved by the ordinary struct lookup further down like any other.

    // "Stack<elem>" is deliberately NOT recognized here anymore (see
    // docs/language/0006-generics.md's own List<T>/Stack<T> port follow-up) - it's a real,
    // user-declared generic struct now (std/collections.ax), resolved by the ordinary struct
    // lookup further down like any other, same as List<T> just above.

    // "LinkedList<elem>" - a doubly linked, node-based collection (see
    // docs/language/0036-linked-lists.md) - same one-level element
    // restriction as List<elem>/Stack<elem> above, for the same reason.
    if (name.starts_with("LinkedList<") && name.back() == '>')
    {
        const std::string elementName = name.substr(11, name.size() - 12);
        const Type elementType = resolveType(elementName);
        return arrayLikeType(TypeKind::LinkedList, typeName(elementType));
    }

    // "Deque<elem>" is deliberately NOT recognized here anymore (see
    // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T> port follow-up) - it's a
    // real, user-declared generic struct now (std/collections.ax), resolved by the ordinary
    // struct lookup further down like any other, same as List<T>/Stack<T> above.

    // "Queue<elem>" is deliberately NOT recognized here anymore (see
    // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T> port follow-up) -
    // it's a real, user-declared generic struct now (std/collections.ax), resolved by the
    // ordinary struct lookup further down like any other, same as List<T>/Stack<T>/Deque<T>
    // above.

    // "PriorityQueue<elem>" is deliberately NOT recognized here anymore (see
    // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T>/PriorityQueue<T>
    // port follow-up) - it's a real, user-declared generic struct now (std/collections.ax),
    // resolved by the ordinary struct lookup further down like any other. The "T must be
    // orderable" restriction this branch used to enforce eagerly, with a dedicated error
    // message, is no longer enforced at construction time - PriorityQueue<bool> now parses/
    // resolves fine as an ordinary generic struct reference; the failure instead surfaces from
    // within the monomorphized push/sift body's own `<` comparison (a generic "comparison
    // requires two orderable values" error, not this dedicated one) - still at compile time, on
    // any PriorityQueue<bool> reference (GenericMonomorphizer eagerly clones every impl method
    // the moment a struct instantiation is referenced, not lazily per call).

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
    // restriction here is orderability, not hashability - same
    // isOrderableKind requirement PriorityQueue<T>/SortedMap<K,V>'s own
    // element/key already have (see docs/language/0039-priority-queues.md).
    if (name.starts_with("SortedSet<") && name.back() == '>')
    {
        const std::string elementName = name.substr(10, name.size() - 11);
        const Type elementType = resolveType(elementName);
        if (!isOrderableKind(elementType.kind))
        {
            throw std::runtime_error(
                "SortedSet<T> requires an orderable element type (i32, i64, f64, char, or str only "
                "in "
                "this phase - no other type is comparable yet), found SortedSet<" +
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
    // key restriction is orderability, not hashability: only types
    // isOrderableKind accepts (i32, char) have a real total order in this
    // language today (same reasoning PriorityQueue<T>'s own element-type
    // restriction already established - see
    // docs/language/0039-priority-queues.md). V has no such requirement
    // (never compared, only stored) and may be any resolvable type except
    // slice<T> (still parameter-only), exactly like Map<K,V>'s own V.
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
        if (!isOrderableKind(keyType.kind))
        {
            throw std::runtime_error("SortedMap<K,V> requires an orderable key type (i32, i64, "
                                     "f64, char, or str only in this "
                                     "phase - no other type is comparable yet), found SortedMap<" +
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

    // "i32|str" - the canonical ("|"-joined, sorted, deduplicated) form
    // Parser::parseTypeName always produces for a "T1 | T2 | ..." union
    // type (see docs/language/0065-unions.md). A real struct/enum/
    // primitive name can never itself contain '|' (not a valid identifier
    // character), so this check is unambiguous.
    if (name.find('|') != std::string::npos)
    {
        return resolveUnionType(name);
    }

    // "fn(T1,T2)->R" - the canonical form Parser::parseTypeName always produces (see
    // docs/language/0067-closures.md). Every param type and the return type are resolved here
    // purely to validate them (an unsupported/unknown one throws the same way any other nested
    // type would), matching Result<T,E>'s own "resolve just to validate, the actual Type stores
    // the canonical string" shape.
    if (name.starts_with("fn("))
    {
        const auto [paramTypeNames, returnTypeName] = closureParamAndReturnTypes(name);
        for (const auto& paramTypeName : paramTypeNames)
        {
            resolveType(paramTypeName);
        }
        resolveType(returnTypeName);
        return simpleType(TypeKind::Closure, name);
    }

    if (const auto it = primitives.find(name); it != primitives.end())
    {
        return simpleType(it->second);
    }
    if (structs_.contains(name))
    {
        return simpleType(TypeKind::Struct, name);
    }
    if (enums_.contains(name))
    {
        return simpleType(TypeKind::Enum, name);
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

        // Array stores its element type as a canonical, re-resolvable string (see
        // docs/language/0052-optional.md's own follow-up, which moved these off their old flat
        // elementKind representation onto Map/Set's). List<T>/Stack<T> are real structs now (see
        // docs/language/0006-generics.md's own List<T>/Stack<T> port follow-up) - hashable via
        // the ordinary TypeKind::Struct case below, like any other struct.
        case TypeKind::Array:
        {
            const Type element = resolveType(type.elementTypeName);
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
            // A generic top-level function template (see docs/language/0006-generics.md's own
            // List<T> port follow-up) is never itself registered - mirrors StructDecl/ImplDecl's
            // own identical typeParams.empty() guard just below/above. Only GenericMonomorphizer's
            // synthesized, concrete-per-call-site clones (plain FunctionDecl items with an empty
            // typeParams of their own) are - picked up by this same branch, on their own.
            if (!function->typeParams.empty())
            {
                continue;
            }
            // "print"/"write" are reserved builtin names (see
            // docs/language/Axea_Printing_Formatting.md) - a real Axea
            // function with either name would otherwise be permanently
            // unreachable, since CallExpr checks the builtin names first.
            if (function->name == "print" || function->name == "write")
            {
                throw std::runtime_error("'" + function->name +
                                         "' is a reserved builtin name and cannot be redefined");
            }
            if (externs_.contains(function->name))
            {
                throw std::runtime_error("'" + function->name +
                                         "' is declared both as a function and as an extern");
            }
            functions_[function->name] = function;
        }
        else if (const auto* structDecl = dynamic_cast<const StructDecl*>(item.get()))
        {
            // A generic struct template (see docs/language/0006-generics.md) is never itself
            // registered - only GenericMonomorphizer's own synthesized, fully concrete
            // instantiations are. Registering the raw template would make its own fields (an
            // unsubstituted type-parameter placeholder like "T") fail every later
            // resolveType(field.type) call unconditionally.
            if (structDecl->typeParams.empty())
            {
                structs_[structDecl->name] = structDecl;
            }
        }
        else if (const auto* enumDecl = dynamic_cast<const EnumDecl*>(item.get()))
        {
            enums_[enumDecl->name] = enumDecl;
        }
        else if (const auto* externDecl = dynamic_cast<const ExternDecl*>(item.get()))
        {
            if (externDecl->name == "print" || externDecl->name == "write")
            {
                throw std::runtime_error("'" + externDecl->name +
                                         "' is a reserved builtin name and cannot be redefined");
            }
            // Rejected explicitly, not left to silently shadow - a real
            // Axea function and an extern c declaration sharing one name
            // would otherwise let CallExpr's own functions_-before-externs_
            // lookup order silently pick one over the other with no
            // warning (see docs/language/0048-ffi.md).
            if (functions_.contains(externDecl->name))
            {
                throw std::runtime_error("'" + externDecl->name +
                                         "' is declared both as a function and as an extern");
            }
            externs_[externDecl->name] = externDecl;
        }
        else if (const auto* traitDecl = dynamic_cast<const TraitDecl*>(item.get()))
        {
            traits_[traitDecl->name] = traitDecl;
        }
        else if (const auto* implDecl = dynamic_cast<const ImplDecl*>(item.get()))
        {
            // A generic impl template (see docs/language/0006-generics.md) is never itself
            // registered - only GenericMonomorphizer's own synthesized, fully concrete methods
            // (plain top-level FunctionDecl items, picked up by the ordinary FunctionDecl
            // branch above) are - mirrors the identical typeParams.empty() guard for StructDecl
            // just above. Registering the raw template's own methods would make `self`'s
            // unsubstituted type text ("Box<T>") fail every later resolveType call.
            if (implDecl->typeParams.empty())
            {
                // Each method is already a real FunctionDecl with a mangled,
                // permanently call-syntax-unreachable name (see ImplDecl's
                // own comment) - folded straight into functions_ exactly
                // like a top-level FunctionDecl, no reserved-name or
                // extern-collision check needed (those only guard against a
                // *user-typed* identifier colliding with a builtin/extern,
                // which a mangled '.'-containing name can never do).
                for (const auto& method : implDecl->methods)
                {
                    functions_[method->name] = method.get();
                }
            }
        }
    }

    // Modules (see docs/language/0066-modules.md) - main.cpp's own module-loading pass has
    // already renamed every function/extern belonging to a module "math" to "math.name" (the
    // exact same '.'-mangling ImplDecl methods already use for "TypeName.methodName" - see that
    // struct's own comment), so every real module name is self-derivable straight from
    // functions_/externs_'s own already-registered keys - no separate input needed from
    // main.cpp. This does mean an impl's own mangled "TypeName.method" keys are picked up here
    // too (indistinguishable from a real module by the name text alone), a harmless quirk: for
    // "TypeName" to actually be treated as a module, a *distinct* real module of that exact name
    // would also need to be loaded, and even then a qualified call through it would just fail
    // ordinary arg-count/type checking (an impl method's own bound `self` has no equivalent in a
    // module-qualified call) rather than silently misbehaving.
    for (const auto& [name, function] : functions_)
    {
        if (const auto dot = name.rfind('.'); dot != std::string::npos)
        {
            moduleNames_.insert(name.substr(0, dot));
        }
    }
    for (const auto& [name, externDecl] : externs_)
    {
        // An extern's own `name` is never module-qualified (see ExternDecl::moduleName's own
        // comment) - its `moduleName` field carries this instead.
        if (!externDecl->moduleName.empty())
        {
            moduleNames_.insert(externDecl->moduleName);
        }
    }

    // Checked before the generic per-function param-type resolution pass
    // just below, so an impl targeting an unknown type reports this
    // specific, friendlier message rather than the generic "unsupported
    // type" a bare resolveType(self's type) would otherwise throw first.
    for (const auto& item : program.items)
    {
        const auto* implDecl = dynamic_cast<const ImplDecl*>(item.get());
        if (!implDecl)
        {
            continue;
        }
        if (implDecl->typeParams.empty())
        {
            if (!structs_.contains(implDecl->typeName))
            {
                throw std::runtime_error("impl target '" + implDecl->typeName +
                                         "' is not a known struct");
            }
            continue;
        }
        // Generic inherent impl (see docs/language/0006-generics.md) - the target is a generic
        // struct template, never itself registered in structs_ (mirrors StructDecl's own
        // typeParams-non-empty exclusion just above). Validated directly against program.items
        // instead, checking arity so a mismatched `impl<T, U> Box<T> { ... }` is caught here
        // rather than silently mis-monomorphizing later.
        const bool targetsKnownGenericStruct = std::any_of(
            program.items.begin(),
            program.items.end(),
            [&](const auto& other)
            {
                const auto* candidate = dynamic_cast<const StructDecl*>(other.get());
                return candidate && candidate->name == implDecl->typeName &&
                       candidate->typeParams.size() == implDecl->typeParams.size();
            });
        if (!targetsKnownGenericStruct)
        {
            throw std::runtime_error(
                "impl target '" + implDecl->typeName + "' is not a known generic struct with " +
                std::to_string(implDecl->typeParams.size()) + " type parameter(s)");
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
    // extern c signatures are validated separately (see
    // docs/language/0048-ffi.md) - every param/return type must be
    // FFI-safe (isFfiSafeType above), a real, narrower restriction than
    // an ordinary Axea function's own param/return type has.
    for (const auto& [name, externDecl] : externs_)
    {
        for (const auto& param : externDecl->params)
        {
            const Type paramType = resolveType(param.type);
            if (!isFfiSafeType(paramType))
            {
                throw std::runtime_error(
                    "extern function '" + externDecl->name +
                    "' has an unsupported parameter type: " + typeName(paramType) +
                    " (only i32, i64, bool, str, cstr, and *T are FFI-safe)");
            }
        }
        if (externDecl->returnType)
        {
            const Type returnType = resolveType(*externDecl->returnType);
            if (!isFfiSafeType(returnType))
            {
                throw std::runtime_error(
                    "extern function '" + externDecl->name +
                    "' has an unsupported return type: " + typeName(returnType) +
                    " (only i32, i64, bool, str, cstr, *T, or no return type/unit "
                    "are FFI-safe)");
            }
        }
    }
    for (const auto& [name, structDecl] : structs_)
    {
        for (const auto& field : structDecl->fields)
        {
            const Type fieldType = resolveType(field.type);
            rejectSliceOutsideParameter(fieldType, "a struct field type");
            rejectMapOrSetAsFieldType(fieldType);
            rejectLinkedListAsFieldType(fieldType);
            rejectSortedMapAsFieldType(fieldType);
            rejectSortedSetAsFieldType(fieldType);
            rejectOwnedStringAsFieldType(fieldType);
            rejectBufferAsFieldType(fieldType);
        }
    }

    // `impl TraitName for TypeName` conformance (see
    // docs/language/0062-display-trait.md) - validated in this same
    // second pass, once every struct/trait name is known. Two checks,
    // both real but deliberately minimal:
    //  1. `typeName` must be a known struct - an impl for an unknown or
    //     non-struct type is always a mistake.
    //  2. If `traitName` matches a real `TraitDecl`, every one of its
    //     declared methods must be implemented with the same name and
    //     parameter count (not full per-parameter type conformance -
    //     see that doc's own Known Imprecision section). `Display`
    //     specifically is additionally required to define a "format"
    //     method with exactly 2 parameters (self, buf) even when no
    //     matching `TraitDecl` was written in source - this is the one
    //     compiler-recognized trait whose shape actually drives runtime
    //     dispatch (print/interpolation), so an `impl Display` with no
    //     usable `format` is always a mistake worth catching here rather
    //     than silently falling back to default struct printing later.
    for (const auto& item : program.items)
    {
        const auto* implDecl = dynamic_cast<const ImplDecl*>(item.get());
        if (!implDecl)
        {
            continue;
        }
        // implDecl->typeName's own "is this a known struct" check already
        // ran above, before the generic per-function param-type
        // resolution pass.
        if (const auto it = traits_.find(implDecl->traitName); it != traits_.end())
        {
            for (const auto& sig : it->second->methods)
            {
                const auto methodIt = std::find_if(
                    implDecl->methods.begin(),
                    implDecl->methods.end(),
                    [&](const auto& m) { return m->name == implDecl->typeName + "." + sig.name; });
                if (methodIt == implDecl->methods.end())
                {
                    throw std::runtime_error("impl " + implDecl->traitName + " for " +
                                             implDecl->typeName + " is missing method '" +
                                             sig.name + "'");
                }
                if ((*methodIt)->params.size() != sig.paramCount)
                {
                    throw std::runtime_error("impl " + implDecl->traitName + " for " +
                                             implDecl->typeName + "'s '" + sig.name + "' has " +
                                             std::to_string((*methodIt)->params.size()) +
                                             " parameter(s), trait '" + implDecl->traitName +
                                             "' declares " + std::to_string(sig.paramCount));
                }
            }
        }
        if (implDecl->traitName == "Display")
        {
            const auto formatIt = std::find_if(
                implDecl->methods.begin(),
                implDecl->methods.end(),
                [&](const auto& m) { return m->name == implDecl->typeName + ".format"; });
            if (formatIt == implDecl->methods.end() || (*formatIt)->params.size() != 2)
            {
                throw std::runtime_error("impl Display for " + implDecl->typeName +
                                         " must define 'format(self, buf: Buffer)'");
            }
        }
    }
}

void TypeChecker::check(const Program& program)
{
    registerSignatures(program);

    TypeEnv globalEnv;
    for (const auto& item : program.items)
    {
        insideUnsafe_ = false; // see insideUnsafe_'s own comment - every top-level item starts
                               // fresh, regardless of what a previous one left it as
        if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get()))
        {
            // A generic top-level function template's own body is never checked directly
            // (mirrors ImplDecl's identical guard just below) - a bare type-parameter reference
            // in it would fail resolveType. Only GenericMonomorphizer's synthesized, concrete
            // clones are checked, each an ordinary FunctionDecl with an empty typeParams.
            if (function->typeParams.empty())
            {
                checkFunction(*function);
            }
        }
        else if (const auto* implDecl = dynamic_cast<const ImplDecl*>(item.get()))
        {
            // A generic impl template's own methods are never checked directly (mirrors
            // registerSignatures's identical guard) - `self`'s unsubstituted type text
            // ("Box<T>") would fail resolveType. Only GenericMonomorphizer's synthesized,
            // concrete clones are checked, and those arrive as ordinary top-level FunctionDecl
            // items, already handled by the branch above.
            if (implDecl->typeParams.empty())
            {
                // Each method is checked exactly like a top-level function -
                // `self`'s type already resolved to a real struct name by the
                // parser, so checkFunction needs no impl-awareness at all.
                for (const auto& method : implDecl->methods)
                {
                    checkFunction(*method);
                }
            }
        }
        else if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(item.get()))
        {
            checkStmt(*assignment, globalEnv, nullptr, nullptr);
        }
        else if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(item.get()))
        {
            // A bare top-level call kept for its side effect
            // (`print("hi")`, `write("...")`) - see
            // Parser::looksLikeFunctionDecl's own doc comment for why
            // this is the one non-assignment top-level statement shape.
            checkStmt(*exprStmt, globalEnv, nullptr, nullptr);
        }
    }
}

void TypeChecker::checkFunction(const FunctionDecl& function)
{
    // Modules (see docs/language/0066-modules.md) - see currentFunctionModule_'s own comment.
    const auto dot = function.name.rfind('.');
    currentFunctionModule_ = dot != std::string::npos ? function.name.substr(0, dot) : "";
    insideUnsafe_ = false; // see insideUnsafe_'s own comment - every function body starts fresh

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
            // `unsafe { return x }` (see docs/language/0019-unsafe.md) - unlike `if`, there's
            // only one branch, so this definitely returns iff its own body does, no
            // branch-merging needed.
            if (const auto* unsafeBlock = dynamic_cast<const UnsafeBlockExpr*>(exprStmt->expr.get());
                unsafeBlock &&
                definitelyReturns(static_cast<const BlockExpr&>(*unsafeBlock->body)))
            {
                return true;
            }
        }
    }
    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(block.result.get()))
    {
        return definitelyReturnsBranch(*ifExpr);
    }
    if (const auto* unsafeBlock = dynamic_cast<const UnsafeBlockExpr*>(block.result.get()))
    {
        return definitelyReturns(static_cast<const BlockExpr&>(*unsafeBlock->body));
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
        // `x: Optional<T> = None` (see docs/language/0052-optional.md) -
        // NoneExpr carries no expression to synthesize a type from, so its
        // type is taken directly from the declared type here rather than
        // through the generic checkExpr(NoneExpr) path (which always
        // throws "cannot infer", since it has no such context available).
        // `x: Result<T,E> = Ok(value)`/`Err(value)` (see
        // docs/language/0063-result.md) - unlike NoneExpr just above,
        // Ok(value)/Err(value) each carry a real expression, but still need
        // this exact same "borrow the *other* type parameter from context"
        // treatment: Ok(value) synthesizes T bottom-up from `value` but has
        // no E to synthesize; Err(value) has the reverse gap. Both are only
        // resolvable here, against a declared Result<T,E> type - anywhere
        // else falls through to checkExpr(OkExpr)/(ErrExpr) below, which
        // always throws "cannot infer" (mirrors NoneExpr's own identical
        // fallback).
        Type valueType;
        if (dynamic_cast<const NoneExpr*>(assignment->value.get()) && assignment->declaredType)
        {
            const Type declared = resolveType(*assignment->declaredType);
            if (declared.kind != TypeKind::Optional)
            {
                throw std::runtime_error("'None' requires a declared Optional<T> type, found " +
                                         typeName(declared));
            }
            valueType = declared;
        }
        else if (const auto* okExpr = dynamic_cast<const OkExpr*>(assignment->value.get());
                 okExpr && assignment->declaredType)
        {
            const Type declared = resolveType(*assignment->declaredType);
            if (declared.kind != TypeKind::Result)
            {
                throw std::runtime_error("'Ok' requires a declared Result<T,E> type, found " +
                                         typeName(declared));
            }
            const Type okType =
                checkExpr(*okExpr->value, env, expectedReturnType, currentLoopBreakTypes);
            if (!(okType == resolveType(declared.elementTypeName)))
            {
                throw std::runtime_error("'Ok' value has type " + typeName(okType) +
                                         " but Result's own Ok type is " +
                                         declared.elementTypeName);
            }
            valueType = declared;
        }
        else if (const auto* errExpr = dynamic_cast<const ErrExpr*>(assignment->value.get());
                 errExpr && assignment->declaredType)
        {
            const Type declared = resolveType(*assignment->declaredType);
            if (declared.kind != TypeKind::Result)
            {
                throw std::runtime_error("'Err' requires a declared Result<T,E> type, found " +
                                         typeName(declared));
            }
            const Type errType =
                checkExpr(*errExpr->value, env, expectedReturnType, currentLoopBreakTypes);
            if (!(errType == resolveType(declared.valueTypeName)))
            {
                throw std::runtime_error("'Err' value has type " + typeName(errType) +
                                         " but Result's own Err type is " + declared.valueTypeName);
            }
            valueType = declared;
        }
        // A bare top-level function name, used where a closure type is declared (see
        // docs/language/0067-closures.md) - checked before the generic checkExpr fallback below,
        // which would otherwise throw "undefined variable" trying to resolve a bare function
        // name as if it were bound to a value.
        else if (assignment->declaredType &&
                 isFunctionRefAssignableToClosure(
                     *assignment->value, resolveType(*assignment->declaredType), env))
        {
            valueType = resolveType(*assignment->declaredType);
        }
        // Self-referential closures (see docs/language/0067-closures.md) - `f`'s own signature
        // is knowable directly from the closure literal itself (its own param/return type text),
        // independent of its own body, so it can be pre-bound in `env` *before* checkExpr checks
        // that body below - letting a self-call like `f(n-1)` inside the body resolve through
        // the exact same "closure-typed local" call path every other closure call already goes
        // through, with no new call-resolution logic needed anywhere. Only when `value` is
        // *directly* a closure literal (not, say, an if-expression that might produce one) -
        // matches this whole feature's existing "closure-typed local" detection, which is
        // likewise keyed off a value's static type, not its runtime shape.
        else if (const auto* closureExpr =
                     dynamic_cast<const ClosureExpr*>(assignment->value.get()))
        {
            env.define(assignment->name, closureSignatureType(*closureExpr));
            valueType =
                checkExpr(*assignment->value, env, expectedReturnType, currentLoopBreakTypes);
        }
        else
        {
            valueType =
                checkExpr(*assignment->value, env, expectedReturnType, currentLoopBreakTypes);
        }
        if (assignment->declaredType)
        {
            const Type declared = resolveType(*assignment->declaredType);
            rejectSliceOutsideParameter(declared, "a local variable's declared type");
            if (!(declared == valueType))
            {
                // Implicit union wrapping (see docs/language/0065-unions.md) -
                // `x: i32 | str = 5` needs no wrapper syntax; the variable is
                // tracked at the declared union type from here on (not the
                // bare alternative's own type), so a later `match x { ... }`
                // sees the union it was actually declared as.
                if (!isUnionMember(valueType, declared))
                {
                    throw std::runtime_error("variable '" + assignment->name + "' declared as " +
                                             typeName(declared) + " but initialized with " +
                                             typeName(valueType));
                }
                valueType = declared;
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
        // `return None` (see docs/language/0052-optional.md) - same
        // "borrow the type from context instead of synthesizing it" special
        // case AssignmentStmt's own NoneExpr handling above needs, using
        // the enclosing function's own declared return type as the context
        // this time. If expectedReturnType isn't itself Optional<U>, this
        // deliberately falls through to the generic checkExpr(NoneExpr)
        // path below, whose "cannot infer" error is the right message.
        // `return Ok(value)`/`Err(value)` (see docs/language/0063-result.md) -
        // same "borrow the missing type parameter from context" treatment
        // AssignmentStmt's own Ok/Err handling above needs, sourced from
        // the enclosing function's own declared return type instead of a
        // local's declared type. Falls through to the generic
        // checkExpr(OkExpr)/(ErrExpr) "cannot infer" error when
        // expectedReturnType isn't itself Result<T,E>, same as None's own
        // fallback just below.
        Type valueType;
        if (returnStmt->value && dynamic_cast<const NoneExpr*>(returnStmt->value.get()) &&
            expectedReturnType->kind == TypeKind::Optional)
        {
            valueType = *expectedReturnType;
        }
        else if (const auto* okExpr = returnStmt->value
                                          ? dynamic_cast<const OkExpr*>(returnStmt->value.get())
                                          : nullptr;
                 okExpr && expectedReturnType->kind == TypeKind::Result)
        {
            const Type okType =
                checkExpr(*okExpr->value, env, expectedReturnType, currentLoopBreakTypes);
            if (!(okType == resolveType(expectedReturnType->elementTypeName)))
            {
                throw std::runtime_error("'Ok' value has type " + typeName(okType) +
                                         " but function's own Result Ok type is " +
                                         expectedReturnType->elementTypeName);
            }
            valueType = *expectedReturnType;
        }
        else if (const auto* errExpr = returnStmt->value
                                           ? dynamic_cast<const ErrExpr*>(returnStmt->value.get())
                                           : nullptr;
                 errExpr && expectedReturnType->kind == TypeKind::Result)
        {
            const Type errType =
                checkExpr(*errExpr->value, env, expectedReturnType, currentLoopBreakTypes);
            if (!(errType == resolveType(expectedReturnType->valueTypeName)))
            {
                throw std::runtime_error("'Err' value has type " + typeName(errType) +
                                         " but function's own Result Err type is " +
                                         expectedReturnType->valueTypeName);
            }
            valueType = *expectedReturnType;
        }
        // A bare top-level function name, used where a closure-typed return is declared (see
        // docs/language/0067-closures.md) - checked before the generic checkExpr fallback below.
        else if (returnStmt->value &&
                 isFunctionRefAssignableToClosure(*returnStmt->value, *expectedReturnType, env))
        {
            valueType = *expectedReturnType;
        }
        else
        {
            valueType =
                returnStmt->value
                    ? checkExpr(*returnStmt->value, env, expectedReturnType, currentLoopBreakTypes)
                    : kUnit;
        }
        if (!(valueType == *expectedReturnType))
        {
            // Implicit union wrapping (see docs/language/0065-unions.md) -
            // `return 5` from a function declared `-> i32 | str` needs no
            // wrapper syntax.
            if (!isUnionMember(valueType, *expectedReturnType))
            {
                throw std::runtime_error("'return' produces " + typeName(valueType) +
                                         " but function declares " + typeName(*expectedReturnType));
            }
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

        const Type elementType = resolveType(objectType.elementTypeName);
        const Type valueType =
            checkExpr(*indexAssign->value, env, expectedReturnType, currentLoopBreakTypes);
        if (!(valueType == elementType))
        {
            throw std::runtime_error("array element expects " + typeName(elementType) + ", got " +
                                     typeName(valueType));
        }
        return;
    }

    if (const auto* derefAssign = dynamic_cast<const DerefAssignStmt*>(&stmt))
    {
        const Type pointerType =
            checkExpr(*derefAssign->pointer, env, expectedReturnType, currentLoopBreakTypes);
        if (pointerType.kind != TypeKind::Pointer)
        {
            throw std::runtime_error("'*' requires a pointer type, got " + typeName(pointerType));
        }
        if (!insideUnsafe_)
        {
            throw std::runtime_error("dereferencing a raw pointer requires an 'unsafe' block");
        }
        const Type pointeeType = resolveType(pointerType.elementTypeName);
        const Type valueType =
            checkExpr(*derefAssign->value, env, expectedReturnType, currentLoopBreakTypes);
        if (!(valueType == pointeeType))
        {
            throw std::runtime_error("pointer dereference assignment expects " +
                                     typeName(pointeeType) + ", got " + typeName(valueType));
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

    // Stack<T> is a real, user-declared generic struct now (see
    // docs/language/0006-generics.md's own List<T>/Stack<T> port follow-up) - its own `.length`
    // is an ordinary method (`s.length()`), not a bare field, since it delegates to an internal
    // `items: List<T>` field with no computed-property syntax to expose that through a bare
    // field read; reached via general struct method dispatch, not here.

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

    // Queue<T> is a real, user-declared generic struct now (see
    // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T> port follow-up) -
    // its own `.length` is an ordinary method (`q.length()`), not a bare field, since it
    // delegates to an internal `items: Deque<T>` field with no computed-property syntax to
    // expose that through a bare field read (the identical reasoning Stack<T>.length() already
    // established); reached via general struct method dispatch, not here.

    // PriorityQueue<T> is a real, user-declared generic struct now (see
    // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T>/PriorityQueue<T>
    // port follow-up) - its own `.length` is an ordinary method (`q.length()`), not a bare
    // field, mirroring Stack<T>/Queue<T>'s own identical composed-collection precedent; reached
    // via general struct method dispatch, not here.

    // Shared<T> auto-derefs to T's own fields (see the move-semantics RFC's own "opaque wrapper,
    // transparent field/method access" design) - re-resolve to the wrapped type and fall through
    // into the exact same struct-field lookup below unchanged. ShareExpr's own checkExpr case
    // already guarantees the wrapped type is Struct or Enum; only the Struct case actually has
    // fields to expose here (an Enum's own fields only make sense behind a `match`, not a bare
    // `.field`, exactly like a plain (unwrapped) enum value already works).
    const Type derefedType =
        objectType.kind == TypeKind::Shared ? resolveType(objectType.elementTypeName) : objectType;

    if (derefedType.kind != TypeKind::Struct)
    {
        throw std::runtime_error("field access on non-struct type " + typeName(derefedType));
    }

    const StructDecl& decl = *structs_.at(derefedType.structName);
    for (const auto& declaredField : decl.fields)
    {
        if (declaredField.name == field)
        {
            return resolveType(declaredField.type);
        }
    }
    throw std::runtime_error("struct '" + derefedType.structName + "' has no field '" + field +
                             "'");
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

    if (dynamic_cast<const Int64Expr*>(&expr))
    {
        return kI64;
    }

    if (dynamic_cast<const FloatExpr*>(&expr))
    {
        return kF64;
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

    // `"Hello {name}, you are {age}."` (see
    // docs/language/Axea_Printing_Formatting.md) - each piece's own expr
    // (literal pieces have none) must be text-representable; the result
    // is always a fresh String, matching that doc's own "when runtime
    // construction is necessary, the result is an owned String" framing -
    // even a piece-free literal never reaches this node at all (the
    // parser only builds one when at least one `{...}` span was found;
    // see Parser::parseStringLiteral).
    if (const auto* interpolated = dynamic_cast<const InterpolatedStringExpr*>(&expr))
    {
        for (const auto& piece : interpolated->pieces)
        {
            if (!piece.expr)
            {
                continue;
            }
            const Type pieceType =
                checkExpr(*piece.expr, env, expectedReturnType, currentLoopBreakTypes);
            if (!isTextRepresentable(pieceType))
            {
                throw std::runtime_error(
                    "interpolation expression has unsupported type " + typeName(pieceType) +
                    " (only i32, i64, f64, bool, char, str, and String are supported this "
                    "phase)");
            }
            // `{expr=}` and `{expr:?}` (see
            // docs/language/0058-debug-formatting.md) need no further
            // validation here beyond the isTextRepresentable check just
            // above: self-doc only prepends a compile-time-known literal
            // prefix (no type dependency at all), and debug mode is
            // defined for every text-representable type identically to
            // the unformatted case (str/String alone differ, by adding
            // quotes) - unlike a numeric format spec, neither narrows
            // which types are legal.
            // `{expr:spec}` (see docs/language/0055-numeric-format-specs.md
            // and docs/language/0057-alignment.md) - a radix conversion
            // (x/X/b/o) requires an integer and forbids a precision; a
            // precision (.N) requires f64; a bare width/zero-pad with no
            // alignment char requires an integer, matching the source
            // doc's own "Numeric Formatting" examples exactly (`{value:05}`
            // on an i32, `{pi:.2}` on an f64). An explicit alignment char
            // (`<`/`>`/`^`) relaxes that last restriction: alignment pads
            // the piece's own already-computed text representation
            // (numeric or not - `{name:<20}` in the source doc's own
            // "Alignment" section aligns a `str`), so it's valid on any
            // isTextRepresentable type - already checked above - not just
            // i32/i64. Radix/precision keep their own type restrictions
            // regardless of whether alignment is also present.
            if (!piece.formatSpec.empty())
            {
                const FormatSpec spec = parseFormatSpec(piece.formatSpec);
                const bool isInt = pieceType == kI32 || pieceType == kI64;
                if (spec.type != '\0')
                {
                    if (!isInt)
                    {
                        throw std::runtime_error("format spec '" + piece.formatSpec +
                                                 "' (radix conversion) requires an i32 or i64 "
                                                 "value, found " +
                                                 typeName(pieceType));
                    }
                    if (spec.precision.has_value())
                    {
                        throw std::runtime_error(
                            "format spec '" + piece.formatSpec +
                            "' cannot combine a radix conversion (x/X/b/o) with a precision");
                    }
                }
                else if (spec.precision.has_value())
                {
                    if (!(pieceType == kF64))
                    {
                        throw std::runtime_error("format spec '" + piece.formatSpec +
                                                 "' (precision) requires an f64 value, found " +
                                                 typeName(pieceType));
                    }
                }
                else if (spec.align == '\0' && !isInt)
                {
                    throw std::runtime_error("format spec '" + piece.formatSpec +
                                             "' (width) requires an i32 or i64 value, found " +
                                             typeName(pieceType));
                }
            }
        }
        return simpleType(TypeKind::OwnedString);
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        return env.get(name->name);
    }

    if (const auto* block = dynamic_cast<const BlockExpr*>(&expr))
    {
        return checkBlock(*block, env, expectedReturnType, currentLoopBreakTypes);
    }

    if (const auto* unsafeBlock = dynamic_cast<const UnsafeBlockExpr*>(&expr))
    {
        // `insideUnsafe_` is a plain member field with manual save/restore here (not a threaded
        // parameter like currentLoopBreakTypes) - this is the *only* place its value ever
        // changes, so a local save/restore is sufficient to handle nesting correctly
        // (`unsafe { unsafe { ... } }`) with zero changes to any other checkExpr/checkStmt/
        // checkBlock call site. checkFunction resets it to false at the start of every function
        // body, so it can never leak from one function into an unrelated one either.
        const bool wasInsideUnsafe = insideUnsafe_;
        insideUnsafe_ = true;
        const Type result = checkBlock(static_cast<const BlockExpr&>(*unsafeBlock->body), env,
                                       expectedReturnType, currentLoopBreakTypes);
        insideUnsafe_ = wasInsideUnsafe;
        return result;
    }

    if (const auto* addressOf = dynamic_cast<const AddressOfExpr*>(&expr))
    {
        const auto* name = dynamic_cast<const NameExpr*>(addressOf->operand.get());
        if (!name)
        {
            throw std::runtime_error(
                "'&' can only be applied to a local variable this phase (struct fields, array/"
                "collection elements, and dereferenced pointers are not yet supported)");
        }
        if (insideClosureBody_)
        {
            throw std::runtime_error("'&' is not yet supported inside a closure body");
        }
        const Type operandType = env.get(name->name); // throws "undefined variable" if unbound
        // No insideUnsafe_ check - unlike DerefExpr just below, taking an address is always
        // safe (see docs/language/0019-unsafe.md's own stated design).
        return arrayLikeType(TypeKind::Pointer, typeName(operandType));
    }

    if (const auto* deref = dynamic_cast<const DerefExpr*>(&expr))
    {
        const Type pointerType =
            checkExpr(*deref->operand, env, expectedReturnType, currentLoopBreakTypes);
        if (pointerType.kind != TypeKind::Pointer)
        {
            throw std::runtime_error("'*' requires a pointer type, got " + typeName(pointerType));
        }
        if (!insideUnsafe_)
        {
            throw std::runtime_error("dereferencing a raw pointer requires an 'unsafe' block");
        }
        return resolveType(pointerType.elementTypeName);
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
        // `callback(x)` where `callback` is a closure-typed local/param (see
        // docs/language/0067-closures.md) - checked before print/write and the ordinary
        // functions_/externs_ lookup below, since a closure call and a real function/extern call
        // share the exact same `Identifier(args)` syntax (Parser builds an ordinary CallExpr
        // either way - see docs/language/0066-modules.md's own MethodCallExpr-reuse precedent for
        // the same "one parsed shape, disambiguated per pass" idea). A local always shadows a
        // top-level function of the same name, the ordinary "inner scope wins" rule - this is the
        // first construct where that can actually matter, since no earlier feature let a local be
        // *called*.
        if (env.contains(call->callee))
        {
            const Type calleeType = env.get(call->callee);
            if (calleeType.kind == TypeKind::Closure)
            {
                const auto [paramTypeNames, returnTypeName] =
                    closureParamAndReturnTypes(calleeType.structName);
                std::vector<Param> syntheticParams;
                syntheticParams.reserve(paramTypeNames.size());
                for (const auto& paramTypeName : paramTypeNames)
                {
                    syntheticParams.push_back(Param{"", paramTypeName, std::nullopt});
                }
                return checkCallArguments(call->callee,
                                          syntheticParams,
                                          std::optional<std::string>(returnTypeName),
                                          call->arguments,
                                          env,
                                          expectedReturnType,
                                          currentLoopBreakTypes);
            }
        }

        // `print`/`write` (see docs/language/Axea_Printing_Formatting.md) -
        // compiler builtins, checked before the ordinary
        // functions_/externs_ lookup below (mirrors `.parse<T>()`'s own
        // "checked first, not tied to one TypeKind" placement). Any
        // number of arguments (including zero - `print()` alone prints
        // just a newline), each independently required to be
        // text-representable - the same restriction interpolation
        // pieces already have, and for the identical reason: neither
        // mechanism yet reuses the top-level binding printer's own
        // full per-type dispatch (structs, arrays, collections).
        // `registerSignatures` already rejects a user function/extern
        // named "print"/"write", so this can never shadow a real
        // declaration.
        if (call->callee == "print" || call->callee == "write")
        {
            for (const auto& argument : call->arguments)
            {
                const Type argType =
                    checkExpr(*argument, env, expectedReturnType, currentLoopBreakTypes);
                // Same allowlist interpolation uses (see
                // isTextRepresentable's own comment) - a struct argument
                // prints directly (see emitPrint, which special-cases
                // isNamedStructPointerType before falling through to
                // stringifyValue), everything else routes through the
                // general stringify-then-"%s" path, same as interpolation.
                if (!isTextRepresentable(argType))
                {
                    throw std::runtime_error("'" + call->callee +
                                             "' argument has unsupported type " +
                                             typeName(argType) +
                                             " (slice<T> is the one remaining unsupported type "
                                             "this phase)");
                }
            }
            return kUnit;
        }

        // extern c functions (see docs/language/0048-ffi.md) share this
        // exact same call-checking logic - both FunctionDecl and
        // ExternDecl happen to have the identical (params, returnType)
        // shape, so a call site needs to know only which one it found,
        // not which kind of declaration it came from.
        const std::vector<Param>* params = nullptr;
        const std::optional<std::string>* returnType = nullptr;

        const auto it = functions_.find(call->callee);
        if (it != functions_.end())
        {
            params = &it->second->params;
            returnType = &it->second->returnType;
        }
        else
        {
            const auto externIt = externs_.find(call->callee);
            if (externIt == externs_.end())
            {
                throw std::runtime_error("undefined function: " + call->callee);
            }
            params = &externIt->second->params;
            returnType = &externIt->second->returnType;
        }

        return checkCallArguments(call->callee,
                                  *params,
                                  *returnType,
                                  call->arguments,
                                  env,
                                  expectedReturnType,
                                  currentLoopBreakTypes);
    }

    if (const auto* methodCall = dynamic_cast<const MethodCallExpr*>(&expr))
    {
        // `EnumName.Variant(args)` construction (see docs/language/0064-enums.md) - checked
        // before the generic `checkExpr(*methodCall->object, ...)` call just below, which would
        // otherwise throw "undefined variable" trying to resolve a bare enum type name as if it
        // were bound to a value. Reuses the ordinary `object.method(args)` postfix shape
        // (see Parser::parsePostfix) with no new grammar - `EnumName` parses as a plain
        // NameExpr, `Variant(...)` as an ordinary method-call tail; only TypeChecker (here),
        // Interpreter, and IrGenerator ever distinguish it from a real method call, each by the
        // identical "is `object` a bare name matching a known enum type" check.
        if (const auto* enumDecl = asEnumTypeName(*methodCall->object))
        {
            const auto variantIt =
                std::find_if(enumDecl->variants.begin(),
                             enumDecl->variants.end(),
                             [&](const EnumVariant& v) { return v.name == methodCall->method; });
            if (variantIt == enumDecl->variants.end())
            {
                throw std::runtime_error("enum '" + enumDecl->name + "' has no variant '" +
                                         methodCall->method + "'");
            }
            if (methodCall->arguments.size() != variantIt->fieldTypes.size())
            {
                throw std::runtime_error(
                    "variant '" + enumDecl->name + "." + methodCall->method + "' expects " +
                    std::to_string(variantIt->fieldTypes.size()) + " argument(s), got " +
                    std::to_string(methodCall->arguments.size()));
            }
            for (std::size_t i = 0; i < methodCall->arguments.size(); ++i)
            {
                const Type argType = checkExpr(
                    *methodCall->arguments[i], env, expectedReturnType, currentLoopBreakTypes);
                const Type fieldType = resolveType(variantIt->fieldTypes[i]);
                if (!(argType == fieldType))
                {
                    throw std::runtime_error("variant '" + enumDecl->name + "." +
                                             methodCall->method + "' argument " +
                                             std::to_string(i) + " has type " + typeName(argType) +
                                             ", expected " + typeName(fieldType));
                }
            }
            return simpleType(TypeKind::Enum, enumDecl->name);
        }

        // `math.sqrt(x)` module-qualified call (see docs/language/0066-modules.md) - checked
        // before the generic checkExpr(*methodCall->object,...) call just below, which would
        // otherwise throw "undefined variable" trying to resolve a bare module name as if it
        // were bound to a value (mirrors the EnumName.Variant check just above exactly - "is
        // object a bare name matching something known, checked before generic resolution").
        // `object`'s own NameExpr already carries the *real* module name by this point (Parser::
        // parsePostfix rewrote any alias at parse time - see aliases_'s own comment), so no
        // alias table is needed here at all.
        if (const auto* moduleName = dynamic_cast<const NameExpr*>(methodCall->object.get());
            moduleName && moduleNames_.contains(moduleName->name))
        {
            const std::string qualifiedName = moduleName->name + "." + methodCall->method;
            const std::vector<Param>* params = nullptr;
            const std::optional<std::string>* returnType = nullptr;
            bool isPublic = false;

            if (const auto it = functions_.find(qualifiedName); it != functions_.end())
            {
                params = &it->second->params;
                returnType = &it->second->returnType;
                isPublic = it->second->isPublic;
            }
            // An extern is looked up by its own *bare* real name (e.g. "sqrt"), not the
            // qualified one - its `name` is never renamed with a module prefix, since it's the
            // real, externally-linked C symbol (see ExternDecl::moduleName's own comment). The
            // qualification is validated here instead, via that field.
            else if (const auto externIt = externs_.find(methodCall->method);
                     externIt != externs_.end() && externIt->second->moduleName == moduleName->name)
            {
                params = &externIt->second->params;
                returnType = &externIt->second->returnType;
                isPublic = externIt->second->isPublic;
            }
            else
            {
                throw std::runtime_error("module '" + moduleName->name + "' has no function '" +
                                         methodCall->method + "'");
            }

            // A module's own qualified self-reference (called from within its own code) is
            // exempt - `pub` gates *external* visibility, not internal use (see
            // currentFunctionModule_'s own comment).
            if (!isPublic && currentFunctionModule_ != moduleName->name)
            {
                throw std::runtime_error("function '" + methodCall->method + "' in module '" +
                                         moduleName->name + "' is private");
            }

            return checkCallArguments(qualifiedName,
                                      *params,
                                      *returnType,
                                      methodCall->arguments,
                                      env,
                                      expectedReturnType,
                                      currentLoopBreakTypes);
        }

        const Type objectType =
            checkExpr(*methodCall->object, env, expectedReturnType, currentLoopBreakTypes);

        // `.clone()` (the move-semantics work's own Shared<T> RFC) - the one method Shared<T>
        // itself defines. Checked before the per-kind dispatch chain, same reasoning as
        // `.parse`/`.to_cstr` just below: Shared<T> has no TypeKind-keyed "new" dispatch section
        // of its own to join (it's only ever constructed via Shared(x), never a `.method()` call).
        // Bumps the refcount and returns a second, independently-valid handle to the exact same
        // allocation - the sole way to get more than one Shared<T> handle (plain assignment is a
        // move, like everywhere else in the language).
        if (objectType.kind == TypeKind::Shared)
        {
            if (methodCall->method != "clone")
            {
                throw std::runtime_error("Shared<" + objectType.elementTypeName +
                                         "> has no method '" + methodCall->method +
                                         "' (did you mean 'clone'?)");
            }
            if (!methodCall->arguments.empty())
            {
                throw std::runtime_error("'clone' expects 0 arguments, got " +
                                         std::to_string(methodCall->arguments.size()));
            }
            return objectType;
        }

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
            // Returns Optional<T>, not T directly (see
            // docs/language/0052-optional.md) - invalid input is now a real
            // None, not a silently-returned fallback like 0/0.0/false.
            if (methodCall->typeArgument == "i32")
            {
                return arrayLikeType(TypeKind::Optional, "i32");
            }
            if (methodCall->typeArgument == "i64")
            {
                return arrayLikeType(TypeKind::Optional, "i64");
            }
            if (methodCall->typeArgument == "f64")
            {
                return arrayLikeType(TypeKind::Optional, "f64");
            }
            if (methodCall->typeArgument == "bool")
            {
                return arrayLikeType(TypeKind::Optional, "bool");
            }
            throw std::runtime_error(
                "parse<" + methodCall->typeArgument +
                "> is not supported - only parse<i32>, parse<i64>, parse<f64>, and "
                "parse<bool> are implemented this phase");
        }

        // `.to_cstr()` (see docs/language/0048-ffi.md) - the sole legal
        // way to obtain a `cstr`, mirroring `.parse<T>()`'s own placement
        // (checked before the object-type-keyed dispatch chain, since it
        // applies across both str-coercible TypeKinds). Representationally
        // a complete no-op (str is already a null-terminated i8*, exactly
        // what cstr is too - see docs/language/0042-string.md's own
        // Design section) - the conversion exists purely at the type
        // level, matching docs/std/strings/0007-ffi.md's own explicit
        // "use to_cstr() for C interop" framing rather than silent
        // interchangeability.
        if (methodCall->method == "to_cstr")
        {
            if (!isStrCoercible(objectType))
            {
                throw std::runtime_error("'to_cstr' requires str, got " + typeName(objectType));
            }
            if (!methodCall->arguments.empty())
            {
                throw std::runtime_error("'to_cstr' expects 0 arguments, got " +
                                         std::to_string(methodCall->arguments.size()));
            }
            return simpleType(TypeKind::CStr);
        }

        // `.unwrap_or(default)`/`.is_some()`/`.is_none()` (see
        // docs/language/0052-optional.md) - the non-propagating, usable-
        // anywhere half of Optional<T>'s API (the propagating half is `?`,
        // TryExpr, checked separately below since it isn't a MethodCallExpr
        // at all). Grouped here (before the per-kind dispatch chain), same
        // reasoning as `.parse`/`.to_cstr`/`.join` above, even though these
        // three are tied to exactly one TypeKind (Optional) - kept out of
        // that chain anyway since Optional<T> has no TypeKind-keyed "new"
        // dispatch section of its own to join (it's never constructed via
        // a `.method()` call, only Some(x)/None/`.parse<T>()`).
        if (methodCall->method == "unwrap_or")
        {
            // Shared by Optional<T> and Result<T,E> (see
            // docs/language/0063-result.md) - `elementTypeName` means "the
            // success-case payload" for both (T for Optional, T/Ok for
            // Result), so the rest of this check is identical either way,
            // no branching needed beyond the initial kind gate.
            if (objectType.kind != TypeKind::Optional && objectType.kind != TypeKind::Result)
            {
                throw std::runtime_error(
                    "'unwrap_or' requires an Optional<T> or Result<T,E>, got " +
                    typeName(objectType));
            }
            if (methodCall->arguments.size() != 1)
            {
                throw std::runtime_error("'unwrap_or' expects 1 argument, got " +
                                         std::to_string(methodCall->arguments.size()));
            }
            const Type payloadType = resolveType(objectType.elementTypeName);
            const Type defaultType = checkExpr(
                *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
            if (!(defaultType == payloadType))
            {
                throw std::runtime_error("'unwrap_or' default has type " + typeName(defaultType) +
                                         " but the success payload is " + typeName(payloadType));
            }
            return payloadType;
        }

        if (methodCall->method == "is_ok" || methodCall->method == "is_err")
        {
            // Result<T,E>'s own non-propagating check (see
            // docs/language/0063-result.md) - deliberately a distinct
            // method name from Optional's `is_some`/`is_none` (mirrors
            // Rust's own naming split) rather than reusing them, even
            // though the underlying check (field 0) is structurally
            // identical either way.
            if (objectType.kind != TypeKind::Result)
            {
                throw std::runtime_error("'" + methodCall->method +
                                         "' requires a Result<T,E>, got " + typeName(objectType));
            }
            if (!methodCall->arguments.empty())
            {
                throw std::runtime_error("'" + methodCall->method + "' expects 0 arguments, got " +
                                         std::to_string(methodCall->arguments.size()));
            }
            return kBool;
        }

        if (methodCall->method == "is_some" || methodCall->method == "is_none")
        {
            if (objectType.kind != TypeKind::Optional)
            {
                throw std::runtime_error("'" + methodCall->method +
                                         "' requires an Optional<T>, "
                                         "got " +
                                         typeName(objectType));
            }
            if (!methodCall->arguments.empty())
            {
                throw std::runtime_error("'" + methodCall->method + "' expects 0 arguments, got " +
                                         std::to_string(methodCall->arguments.size()));
            }
            return kBool;
        }

        // `.join(separator)` (see
        // docs/language/0050-collection-join-and-slicing.md) - applies
        // across Array, List<T>, and slice<T> alike (mirrors `.parse`/
        // `.to_cstr`'s own placement here, before the per-kind dispatch
        // chain below, since it too applies across more than one
        // TypeKind), T restricted to isTextRepresentable. slice<T> joined
        // docs/language/0056-slice-printing.md - the interpreter's own
        // asIndexable already handled SliceInstance, and
        // resolveIndexableView gained a matching by-value branch, so this
        // was purely a TypeChecker-level restriction, not a missing
        // runtime capability. Always returns a fresh, owned String, built
        // the same way interpolation builds one.
        // `objectType.kind != TypeKind::Struct` guards this (see
        // docs/language/0006-generics.md's own List<T> port follow-up) so a real struct that
        // happens to declare its own "join" method (unlikely, but not this check's business to
        // rule out) falls through to the general struct method dispatch below instead of being
        // permanently shadowed by this builtin - List<T> itself no longer defines "join" at all.
        if (methodCall->method == "join" && objectType.kind != TypeKind::Struct)
        {
            if (objectType.kind != TypeKind::Array && objectType.kind != TypeKind::Slice)
            {
                throw std::runtime_error("'join' requires an Array or slice<T>, got " +
                                         typeName(objectType));
            }
            const Type elementType = resolveType(objectType.elementTypeName);
            if (!isTextRepresentable(elementType))
            {
                throw std::runtime_error(
                    "'join' requires i32/bool/char/str/String elements, found " +
                    typeName(elementType));
            }
            if (methodCall->arguments.size() != 1)
            {
                throw std::runtime_error("'join' expects 1 argument, got " +
                                         std::to_string(methodCall->arguments.size()));
            }
            const Type sepType = checkExpr(
                *methodCall->arguments.front(), env, expectedReturnType, currentLoopBreakTypes);
            if (!isStrCoercible(sepType))
            {
                throw std::runtime_error("'join' separator must be str, got " + typeName(sepType));
            }
            return simpleType(TypeKind::OwnedString);
        }

        // `List<T>` is a real, user-declared generic struct now (see
        // docs/language/0006-generics.md's own List<T> port follow-up) - there is no
        // TypeKind::List-specific push/pop dispatch left here; `.push`/`.pop`/`.get`/`.set`
        // reach it via the general struct method dispatch at the end of this if-chain.

        // Stack<T> is a real, user-declared generic struct now (see
        // docs/language/0006-generics.md's own List<T>/Stack<T> port follow-up) - there is no
        // TypeKind::Stack-specific push/pop/peek dispatch left here; `.push`/`.pop`/`.peek`/
        // `.length()` reach it via the general struct method dispatch at the end of this
        // if-chain.

        // LinkedList<T> (see docs/language/0036-linked-lists.md) - push_front/push_back/
        // pop_front/pop_back are unique names nothing else uses, so there's no ambiguity to
        // resolve downstream (see IrGenerator). No peek_front/peek_back this phase - every operation
        // either adds or removes, never aliases, so RegionChecker needs no
        // exception clause for LinkedList<T> at all.
        if (objectType.kind == TypeKind::LinkedList)
        {
            const Type elementType = resolveType(objectType.elementTypeName);

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

        // Deque<T> is a real, user-declared generic struct now (see
        // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T> port follow-up) - there
        // is no TypeKind::Deque-specific push_front/push_back/pop_front/pop_back dispatch left
        // here; those, plus `.get(i)`/`.set(i, v)` (replacing `[i]`/`[i]=` - Deque<T> is no
        // longer indexable, see isIndexable's own comment), reach it via the general struct
        // method dispatch at the end of this if-chain.

        // Queue<T> is a real, user-declared generic struct now (see
        // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T> port
        // follow-up) - there is no TypeKind::Queue-specific `enqueue`/`dequeue` dispatch left
        // here; both reach it via the general struct method dispatch earlier in this same
        // if-chain.

        // PriorityQueue<T> is a real, user-declared generic struct now (see
        // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T>/
        // PriorityQueue<T> port follow-up) - there is no TypeKind::PriorityQueue-specific
        // `push`/`pop`/`peek` dispatch left here; all three reach it via the general struct
        // method dispatch earlier in this same if-chain (the same one Stack<T>'s own former
        // push/pop/peek already reaches).

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
        // returns it as a String. `write` (see
        // docs/language/0061-buffer-write.md) is a plain same-behavior
        // alias of `append` - Axea's string interpolation already lowers
        // at parse time, for *any* string literal argument regardless of
        // which method receives it, so there is no runtime difference
        // between `buf.append("Age: {age}")` and a hypothetical distinct
        // "formatted write" - `write` exists purely so callers can use
        // the source doc's own naming convention (`append` = direct,
        // `write` = formatted) without it meaning anything different.
        if (objectType.kind == TypeKind::Buffer)
        {
            if (methodCall->method == "append" || methodCall->method == "append_line" ||
                methodCall->method == "write")
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

        // General method-call dispatch for struct types (see docs/language/0006-generics.md's
        // own generic-methods follow-up) - looks up "TypeName.method" in functions_, the exact
        // map ImplDecl's own registerSignatures loop already folds every method into (trait or
        // inherent, concrete or a GenericMonomorphizer-synthesized generic-struct
        // instantiation - all arrive here identically, since by this point functions_ only ever
        // holds concrete, fully-typed FunctionDecls). `self` (the receiver, already
        // type-checked above as `objectType`) is params[0] and is not part of
        // methodCall->arguments, so only params[1:] are checked against it. A method with zero
        // params (no `self` at all - a static/associated function, out of scope this phase) is
        // deliberately excluded here and falls through to the catch-all below.
        if (objectType.kind == TypeKind::Struct)
        {
            if (const auto it = functions_.find(objectType.structName + "." + methodCall->method);
                it != functions_.end() && !it->second->params.empty())
            {
                const std::vector<Param> paramsWithoutSelf(it->second->params.begin() + 1,
                                                            it->second->params.end());
                return checkCallArguments(objectType.structName + "." + methodCall->method,
                                          paramsWithoutSelf,
                                          it->second->returnType,
                                          methodCall->arguments,
                                          env,
                                          expectedReturnType,
                                          currentLoopBreakTypes);
            }
        }

        throw std::runtime_error("no such method '" + methodCall->method + "' on " +
                                 typeName(objectType));
    }

    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        // `EnumName.Variant` (no parens - a no-payload variant, see
        // docs/language/0064-enums.md and MethodCallExpr's own identical check just above) -
        // `object.field` with no trailing `(` parses as FieldExpr, not MethodCallExpr (see
        // Parser::parsePostfix's own `isCall` decision), so a no-payload variant reached this
        // way needs its own interception here.
        if (const auto* enumDecl = asEnumTypeName(*field->object))
        {
            const auto variantIt =
                std::find_if(enumDecl->variants.begin(),
                             enumDecl->variants.end(),
                             [&](const EnumVariant& v) { return v.name == field->field; });
            if (variantIt == enumDecl->variants.end())
            {
                throw std::runtime_error("enum '" + enumDecl->name + "' has no variant '" +
                                         field->field + "'");
            }
            if (!variantIt->fieldTypes.empty())
            {
                throw std::runtime_error(
                    "variant '" + enumDecl->name + "." + field->field + "' requires " +
                    std::to_string(variantIt->fieldTypes.size()) + " argument(s) - use " +
                    enumDecl->name + "." + field->field + "(...)");
            }
            return simpleType(TypeKind::Enum, enumDecl->name);
        }
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
                             typeName(elementType),
                             static_cast<int>(arrayLiteral->elements.size()));
    }

    if (const auto* linkedListNew = dynamic_cast<const LinkedListNewExpr*>(&expr))
    {
        const Type elementType = resolveType(linkedListNew->elementType);
        return arrayLikeType(TypeKind::LinkedList, typeName(elementType));
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
        // "SortedSet<elem>" - same delegation SortedMapNewExpr makes above
        // (see docs/language/0041-sorted-sets.md).
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

        // Single-character indexing (`s[i]`) on a str-coercible object - a
        // real Unicode *codepoint* index, matching `.length`'s own
        // codepoint-not-byte counting (see docs/language/0047-unicode.md),
        // not a byte offset. Checked before (and instead of) `isIndexable`
        // deliberately - str/String are immutable, so this stays absent
        // from `isIndexable` itself (shared with IndexAssignStmt, which
        // must keep rejecting `s[i] = ...`), and str/String don't have an
        // elementTypeName the Array/Slice branch below relies on for its
        // own return type.
        if (isStrCoercible(objectType))
        {
            const Type indexType =
                checkExpr(*index->index, env, expectedReturnType, currentLoopBreakTypes);
            if (!(indexType == kI32))
            {
                throw std::runtime_error("string index must be i32, found " + typeName(indexType));
            }
            return kChar;
        }

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

        return resolveType(objectType.elementTypeName);
    }

    if (const auto* strSlice = dynamic_cast<const StrSliceExpr*>(&expr))
    {
        // Originally str-coercible-only (str or String); briefly widened in
        // docs/language/0050-collection-join-and-slicing.md to also accept an Array (result:
        // a fresh List<T>) - narrowed back to str-coercible-only now that List<T> is a real,
        // user-declared generic struct (see docs/language/0006-generics.md's own List<T> port
        // follow-up): producing one requires a real `newList<T>()` call + a push loop, not just
        // a type-checked bulk copy, and slicing shouldn't grow a hidden dependency on
        // `std/collections.ax` being in scope. Array slicing may return once there's a real need
        // for it, built on top of the actual `newList<T>()`/`.push` API rather than an internal
        // shortcut.
        const Type objectType =
            checkExpr(*strSlice->object, env, expectedReturnType, currentLoopBreakTypes);
        if (!isStrCoercible(objectType))
        {
            throw std::runtime_error("slicing requires str, got " + typeName(objectType));
        }
        const Type resultType = kStr;
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
        return resultType;
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
                // `ptr + i` / `ptr - i` (see docs/language/0019-unsafe.md) - element-scaled
                // pointer arithmetic, legal only inside `unsafe { }`. Checked here rather than in
                // a dedicated AST node: this reuses the existing BinaryExpr(Plus/Minus) shape
                // verbatim, exactly like every other binary operator already does.
                if (leftType.kind == TypeKind::Pointer)
                {
                    if (!insideUnsafe_)
                    {
                        throw std::runtime_error("pointer arithmetic requires an 'unsafe' block");
                    }
                    if (rightType.kind != TypeKind::I32 && rightType.kind != TypeKind::I64)
                    {
                        throw std::runtime_error(
                            "pointer arithmetic offset must be i32 or i64, found " +
                            typeName(rightType));
                    }
                    return leftType;
                }
                return requireInt(leftType, rightType);
            case TokenKind::Star:
            case TokenKind::Slash: return requireInt(leftType, rightType);
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

    // `<expr> as <targetType>` (see docs/language/0005-type-system.md) -
    // both the operand's own type and the target must be numeric
    // (isNumericKind: i32, i64, or f64) - no casting to/from bool/char/
    // str/struct/collections this phase, and no wrapping/checked/
    // saturating variants, just a direct value conversion. Same-kind casts
    // (`i32 as i32`) are allowed too, same as this checker's general
    // "reject only what's actually wrong" philosophy - a redundant cast is
    // harmless, not an error.
    if (const auto* cast = dynamic_cast<const CastExpr*>(&expr))
    {
        const Type operandType =
            checkExpr(*cast->operand, env, expectedReturnType, currentLoopBreakTypes);
        const Type targetType = resolveType(cast->targetType);
        // Pointer-to-pointer cast (see docs/language/0006-generics.md's own List<T> port
        // follow-up) - a raw pointer reinterpretation, legal only inside `unsafe {}` (mirrors
        // DerefExpr's own gating - creating/reading through a reinterpreted pointer is exactly
        // as unchecked as any other raw-pointer operation). Unconditionally allowed between any
        // two element types once inside unsafe - the whole point is reinterpreting bytes.
        if (operandType.kind == TypeKind::Pointer && targetType.kind == TypeKind::Pointer)
        {
            if (!insideUnsafe_)
            {
                throw std::runtime_error("pointer cast requires an 'unsafe' block");
            }
            return targetType;
        }
        if (!isNumericKind(operandType.kind) || !isNumericKind(targetType.kind))
        {
            throw std::runtime_error("'as' cast requires both sides to be i32, i64, or f64, "
                                     "found " +
                                     typeName(operandType) + " as " + typeName(targetType));
        }
        return targetType;
    }

    if (const auto* sizeOf = dynamic_cast<const SizeOfExpr*>(&expr))
    {
        resolveType(sizeOf->typeName); // validates the type exists; result unused otherwise
        return kI64;
    }

    if (const auto* someExpr = dynamic_cast<const SomeExpr*>(&expr))
    {
        const Type payloadType =
            checkExpr(*someExpr->value, env, expectedReturnType, currentLoopBreakTypes);
        return arrayLikeType(TypeKind::Optional, typeName(payloadType));
    }

    if (const auto* shareExpr = dynamic_cast<const ShareExpr*>(&expr))
    {
        const Type payloadType =
            checkExpr(*shareExpr->value, env, expectedReturnType, currentLoopBreakTypes);
        if (payloadType.kind != TypeKind::Struct && payloadType.kind != TypeKind::Enum)
        {
            throw std::runtime_error("Shared<T> requires a struct or enum type, found " +
                                     typeName(payloadType));
        }
        return arrayLikeType(TypeKind::Shared, typeName(payloadType));
    }

    if (dynamic_cast<const NoneExpr*>(&expr))
    {
        // A bare `None` carries no expression to synthesize a payload type
        // from - only reachable here when it appears somewhere checkStmt's
        // AssignmentStmt/ReturnStmt cases don't already special-case it
        // (see docs/language/0052-optional.md), e.g. as a function-call
        // argument or nested inside another expression.
        throw std::runtime_error(
            "cannot infer the type of 'None' here - use it in a declared-type assignment "
            "(x: Optional<T> = None) or a bare 'return None' inside a function declared to "
            "return Optional<T>");
    }

    if (dynamic_cast<const OkExpr*>(&expr) || dynamic_cast<const ErrExpr*>(&expr))
    {
        // Same "cannot infer standalone" fallback NoneExpr above has, for
        // the same structural reason (see docs/language/0063-result.md and
        // OkExpr/ErrExpr's own comment in Expr.hpp): only reachable here
        // when checkStmt's AssignmentStmt/ReturnStmt cases don't already
        // intercept it against a declared Result<T,E> context.
        const bool isOk = dynamic_cast<const OkExpr*>(&expr) != nullptr;
        throw std::runtime_error(
            std::string("cannot infer the type of '") + (isOk ? "Ok" : "Err") +
            "' here - use it in a declared-type assignment (x: Result<T,E> = " +
            (isOk ? "Ok" : "Err") + "(...)) or a bare 'return " + (isOk ? "Ok" : "Err") +
            "(...)' inside a function declared to return Result<T,E>");
    }

    if (const auto* tryExpr = dynamic_cast<const TryExpr*>(&expr))
    {
        // Generalized to Result<T,E> (see docs/language/0063-result.md) -
        // `?` is legal inside a function returning Optional<U> (operand
        // must be Optional<T>) or Result<U,E> (operand must be
        // Result<T,E>, with T,E's own E matching the function's own E
        // exactly - no automatic error-type conversion this phase, see
        // that doc's own Known Imprecision).
        if (!expectedReturnType || (expectedReturnType->kind != TypeKind::Optional &&
                                    expectedReturnType->kind != TypeKind::Result))
        {
            throw std::runtime_error("'?' can only be used inside a function whose own return "
                                     "type is Optional<T> or Result<T,E>");
        }
        const Type operandType =
            checkExpr(*tryExpr->operand, env, expectedReturnType, currentLoopBreakTypes);
        if (operandType.kind == TypeKind::Optional)
        {
            if (expectedReturnType->kind != TypeKind::Optional)
            {
                throw std::runtime_error("'?' operand is " + typeName(operandType) +
                                         " but the enclosing function returns " +
                                         typeName(*expectedReturnType));
            }
            return resolveType(operandType.elementTypeName);
        }
        if (operandType.kind == TypeKind::Result)
        {
            if (expectedReturnType->kind != TypeKind::Result)
            {
                throw std::runtime_error("'?' operand is " + typeName(operandType) +
                                         " but the enclosing function returns " +
                                         typeName(*expectedReturnType));
            }
            if (operandType.valueTypeName != expectedReturnType->valueTypeName)
            {
                throw std::runtime_error("'?' operand's Err type (" + operandType.valueTypeName +
                                         ") doesn't match the enclosing function's own Err type (" +
                                         expectedReturnType->valueTypeName +
                                         ") - no automatic error-type conversion this phase");
            }
            return resolveType(operandType.elementTypeName);
        }
        throw std::runtime_error("'?' requires an Optional<T> or Result<T,E>, got " +
                                 typeName(operandType));
    }

    if (const auto* matchExpr = dynamic_cast<const MatchExpr*>(&expr))
    {
        // See docs/language/0064-enums.md. A real, if simple, exhaustiveness check: every
        // arm's variant name must be a real variant of the scrutinee's own enum, matched at
        // most once, and either every variant is covered by name or a wildcard ('_') arm - which
        // must be the last arm, since anything after it would be unreachable dead code - covers
        // the rest. All arm bodies must produce exactly the same type (no implicit widening,
        // matching this codebase's existing stance on every other branch-shaped construct).
        const Type scrutineeType =
            checkExpr(*matchExpr->scrutinee, env, expectedReturnType, currentLoopBreakTypes);
        if (scrutineeType.kind != TypeKind::Enum)
        {
            throw std::runtime_error("'match' requires an enum value, got " +
                                     typeName(scrutineeType));
        }
        if (matchExpr->arms.empty())
        {
            throw std::runtime_error("'match' expression must have at least one arm");
        }
        const EnumDecl& enumDecl = *enums_.at(scrutineeType.structName);

        bool haveResultType = false;
        Type resultType{};
        std::unordered_set<std::string> coveredVariants;
        bool sawWildcard = false;
        for (const auto& arm : matchExpr->arms)
        {
            if (sawWildcard)
            {
                throw std::runtime_error(
                    "wildcard arm '_' must be the last arm in a 'match' expression");
            }
            TypeEnv armEnv(&env);
            if (arm.variantName == "_")
            {
                sawWildcard = true;
                if (!arm.bindingNames.empty())
                {
                    throw std::runtime_error("wildcard arm '_' cannot bind any names");
                }
            }
            else
            {
                const auto variantIt =
                    std::find_if(enumDecl.variants.begin(),
                                 enumDecl.variants.end(),
                                 [&](const EnumVariant& v) { return v.name == arm.variantName; });
                if (variantIt == enumDecl.variants.end())
                {
                    throw std::runtime_error("enum '" + enumDecl.name + "' has no variant '" +
                                             arm.variantName + "'");
                }
                if (!coveredVariants.insert(arm.variantName).second)
                {
                    throw std::runtime_error("variant '" + arm.variantName +
                                             "' matched more than once in this 'match' "
                                             "expression");
                }
                if (arm.bindingNames.size() != variantIt->fieldTypes.size())
                {
                    throw std::runtime_error("match arm '" + arm.variantName + "' expects " +
                                             std::to_string(variantIt->fieldTypes.size()) +
                                             " binding(s), got " +
                                             std::to_string(arm.bindingNames.size()));
                }
                for (std::size_t i = 0; i < arm.bindingNames.size(); ++i)
                {
                    armEnv.define(arm.bindingNames[i], resolveType(variantIt->fieldTypes[i]));
                }
            }
            const Type armType =
                checkExpr(*arm.body, armEnv, expectedReturnType, currentLoopBreakTypes);
            if (!haveResultType)
            {
                resultType = armType;
                haveResultType = true;
            }
            else if (!(armType == resultType))
            {
                throw std::runtime_error("match arms have incompatible types: " +
                                         typeName(resultType) + " vs " + typeName(armType));
            }
        }
        if (!sawWildcard && coveredVariants.size() != enumDecl.variants.size())
        {
            std::string missing;
            for (const auto& variant : enumDecl.variants)
            {
                if (!coveredVariants.contains(variant.name))
                {
                    if (!missing.empty())
                    {
                        missing += ", ";
                    }
                    missing += variant.name;
                }
            }
            throw std::runtime_error("non-exhaustive match on '" + enumDecl.name +
                                     "' - missing variant(s): " + missing);
        }
        return resultType;
    }

    if (const auto* closureExpr = dynamic_cast<const ClosureExpr*>(&expr))
    {
        // A real closure (see docs/language/0067-closures.md): `closureEnv`'s own parent is the
        // *enclosing* scope, not null - the entire point of "closing over" it, unlike
        // checkFunction's own top-level `TypeEnv env; // no parent: functions don't see top-level
        // globals`. No explicit capture-list bookkeeping is needed here at all: an ordinary
        // NameExpr lookup for a captured name just walks up through `closureEnv` to `env`
        // exactly like a nested block/if-branch already does; only IrGenerator/Interpreter (the
        // passes that actually build a runtime captures struct) need their own explicit
        // free-variable scan.
        TypeEnv closureEnv(&env);
        std::vector<std::string> paramTypeNames;
        paramTypeNames.reserve(closureExpr->params.size());
        for (const auto& param : closureExpr->params)
        {
            // Struct-typed closure params (see docs/language/0067-closures.md) - CapabilityChecker
            // gives each closure literal a synthetic FunctionDecl of its own, reusing its entire
            // per-top-level-function read/write/take inference machinery unchanged (see that
            // doc's own Known Imprecision entry for this).
            const Type paramType = resolveType(param.type);
            closureEnv.define(param.name, paramType);
            paramTypeNames.push_back(typeName(paramType));
        }

        const Type expectedReturn =
            closureExpr->returnType ? resolveType(*closureExpr->returnType) : kUnit;
        const auto& block = static_cast<const BlockExpr&>(*closureExpr->body);
        // `&name` inside a closure body is rejected (see insideClosureBody_'s own comment) -
        // same manual save/restore shape insideUnsafe_ already uses, just gating a rejection.
        const bool wasInsideClosureBody = insideClosureBody_;
        insideClosureBody_ = true;
        checkBlock(block, closureEnv, &expectedReturn, nullptr);
        insideClosureBody_ = wasInsideClosureBody;
        if (!(expectedReturn == kUnit) && !definitelyReturns(block))
        {
            throw std::runtime_error("closure does not return a value of type " +
                                     typeName(expectedReturn) +
                                     " on all paths (did you forget 'return'?)");
        }

        return closureSignatureType(*closureExpr);
    }

    throw std::runtime_error("unsupported expression");
}
