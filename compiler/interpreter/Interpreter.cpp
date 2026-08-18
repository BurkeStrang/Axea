#include "interpreter/Interpreter.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace
{
    // Ascending min-heap comparator for PriorityQueueInstance (see
    // docs/language/0039-priority-queues.md) - T is i32 only this phase, so
    // this compares the std::int64_t payload directly rather than needing a
    // generic Value comparator. std::push_heap/std::pop_heap are max-heaps
    // by their own default (comparator returns true when the *first*
    // argument is lower priority); reversing the comparison (">" instead of
    // "<") makes the smallest element the one that keeps rising to the
    // front, exactly the "ascending, smallest-first" choice this phase
    // makes at the LLVM-backend level too.
    bool priorityQueueLess(const Value& a, const Value& b)
    {
        return std::get<std::int64_t>(a) > std::get<std::int64_t>(b);
    }

    // Also accepts char32_t (returning its own codepoint numerically) -
    // used both for real i32 arithmetic and for char's own ordering
    // comparisons (see docs/language/0044-char.md), which share this same
    // helper. TypeChecker's own requireInt/requireOrdered split already
    // guarantees a char value only ever reaches this via the ordering
    // path, never arithmetic - a well-typed program never asks for
    // char + char.
    std::int64_t asInt(const Value& value)
    {
        if (const auto* integer = std::get_if<std::int64_t>(&value))
        {
            return *integer;
        }
        if (const auto* character = std::get_if<char32_t>(&value))
        {
            return static_cast<std::int64_t>(*character);
        }
        throw std::runtime_error("expected an integer operand");
    }

    // Resolves a str-coercible Value (a bare str, or a String - see
    // TypeChecker::isStrCoercible's identical rule at the type-checking
    // layer, docs/language/0042-string.md) down to its raw content. Always
    // a copy, matching how a plain `str` Value is already copied by value
    // everywhere else in this interpreter.
    std::string asStrContent(const Value& value)
    {
        if (const auto* str = std::get_if<std::string>(&value))
        {
            return *str;
        }
        if (const auto* string = std::get_if<std::shared_ptr<StringInstance>>(&value))
        {
            return (*string)->data;
        }
        throw std::runtime_error("expected a str or String operand");
    }

    // A pointer into an ArrayInstance's, SliceInstance's, or ListInstance's
    // backing storage, plus the effective length to bounds-check against - a
    // slice's `length` may in principle differ from its backing array's own
    // size (though in this whole-array-only-conversion phase they always
    // agree - see docs/language/0032-slices.md). Shared by IndexExpr,
    // IndexAssignStmt, and FieldExpr's ".length" case so array/slice/List
    // indexing and length reads don't need three separate near-duplicate
    // implementations each.
    struct Indexable
    {
        std::vector<Value>* elements;
        std::size_t length;
    };

    std::optional<Indexable> asIndexable(Value& value)
    {
        if (auto* array = std::get_if<std::shared_ptr<ArrayInstance>>(&value))
        {
            return Indexable{&(*array)->elements, (*array)->elements.size()};
        }
        if (auto* slice = std::get_if<std::shared_ptr<SliceInstance>>(&value))
        {
            return Indexable{&(*slice)->backing->elements, (*slice)->length};
        }
        if (auto* list = std::get_if<std::shared_ptr<ListInstance>>(&value))
        {
            return Indexable{&(*list)->elements, (*list)->elements.size()};
        }
        if (auto* deque = std::get_if<std::shared_ptr<DequeInstance>>(&value))
        {
            return Indexable{&(*deque)->elements, (*deque)->elements.size()};
        }
        return std::nullopt;
    }

    bool asBool(const Value& value)
    {
        if (const auto* boolean = std::get_if<bool>(&value))
        {
            return *boolean;
        }
        throw std::runtime_error("expected a boolean condition");
    }

    // Thrown by `return` and caught at the function-call boundary; deliberately
    // does not derive from std::exception so it can't be caught by generic
    // exception handlers along the way.
    struct ReturnSignal
    {
        Value value;
    };

    // Thrown by `break`/`continue` and caught by the nearest enclosing loop's
    // own execution - C++'s normal exception propagation already finds the
    // *innermost* enclosing loop first, so nesting needs no extra bookkeeping
    // (same non-std::exception design as ReturnSignal, for the same reason).
    struct BreakSignal
    {
        Value value;
    };

    struct ContinueSignal
    {
    };

    // Encodes a single Unicode scalar value into its own UTF-8 byte
    // sequence (see docs/language/0044-char.md) - mirrors
    // LlvmIrEmitter::encodeCharUtf8's own bit-for-bit encoding rules
    // exactly (same shift/mask/tag values per byte-length range), just as
    // plain C++ instead of hand-emitted LLVM IR, so the interpreter and
    // the compiled backend agree byte-for-byte on every char's printed
    // form.
    std::string encodeUtf8(char32_t codepoint)
    {
        const std::uint32_t cp = static_cast<std::uint32_t>(codepoint);
        std::string out;
        if (cp <= 0x7F)
        {
            out += static_cast<char>(cp);
        }
        else if (cp <= 0x7FF)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp <= 0xFFFF)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return out;
    }
} // namespace

std::string toString(const Value& value)
{
    if (const auto* integer = std::get_if<std::int64_t>(&value))
    {
        return std::to_string(*integer);
    }
    if (const auto* boolean = std::get_if<bool>(&value))
    {
        return *boolean ? "true" : "false";
    }
    if (const auto* character = std::get_if<char32_t>(&value))
    {
        return encodeUtf8(*character);
    }
    if (const auto* string = std::get_if<std::string>(&value))
    {
        return *string;
    }
    if (const auto* ownedString = std::get_if<std::shared_ptr<StringInstance>>(&value))
    {
        // Bare content, same as `str` above - a String prints exactly like
        // the text it holds, no wrapper/quoting (see
        // docs/language/0042-string.md).
        return (*ownedString)->data;
    }
    if (const auto* buffer = std::get_if<std::shared_ptr<BufferInstance>>(&value))
    {
        // Bare content, same as String above (see docs/language/0043-buffer.md).
        return (*buffer)->data;
    }
    if (const auto* instance = std::get_if<std::shared_ptr<StructInstance>>(&value))
    {
        std::string result = (*instance)->typeName + " { ";
        for (std::size_t i = 0; i < (*instance)->fields.size(); ++i)
        {
            if (i > 0)
            {
                result += ", ";
            }
            result += (*instance)->fields[i].first + ": " + toString((*instance)->fields[i].second);
        }
        result += " }";
        return result;
    }
    if (const auto* array = std::get_if<std::shared_ptr<ArrayInstance>>(&value))
    {
        std::string result = "[";
        for (std::size_t i = 0; i < (*array)->elements.size(); ++i)
        {
            if (i > 0)
            {
                result += ", ";
            }
            result += toString((*array)->elements[i]);
        }
        result += "]";
        return result;
    }
    if (const auto* slice = std::get_if<std::shared_ptr<SliceInstance>>(&value))
    {
        // Provably unreachable in a well-typed program (slice<T> can never
        // be a function return type - see docs/language/0032-slices.md), so
        // a slice value can never actually surface here. Handled anyway,
        // identically to an array, rather than silently falling through to
        // the "()" case below if that invariant were ever violated by a bug.
        std::string result = "[";
        for (std::size_t i = 0; i < (*slice)->length; ++i)
        {
            if (i > 0)
            {
                result += ", ";
            }
            result += toString((*slice)->backing->elements[i]);
        }
        result += "]";
        return result;
    }
    if (const auto* list = std::get_if<std::shared_ptr<ListInstance>>(&value))
    {
        std::string result = "[";
        for (std::size_t i = 0; i < (*list)->elements.size(); ++i)
        {
            if (i > 0)
            {
                result += ", ";
            }
            result += toString((*list)->elements[i]);
        }
        result += "]";
        return result;
    }
    if (const auto* stack = std::get_if<std::shared_ptr<StackInstance>>(&value))
    {
        // Same bracket format as List above - a Stack's order is just as
        // well-defined (bottom to top), so there's no reason to hide
        // contents the way Map/Set's unordered-and-no-iteration-yet case
        // does (see docs/language/0035-stacks.md).
        std::string result = "[";
        for (std::size_t i = 0; i < (*stack)->elements.size(); ++i)
        {
            if (i > 0)
            {
                result += ", ";
            }
            result += toString((*stack)->elements[i]);
        }
        result += "]";
        return result;
    }
    if (const auto* deque = std::get_if<std::shared_ptr<DequeInstance>>(&value))
    {
        // Same bracket format as List/Stack above (not LinkedList/Map/Set's
        // count-only fallback) - a Deque's growable-array-with-a-start-
        // offset representation directly supports full-content printing,
        // matching the LLVM backend's own choice (see
        // docs/language/0037-deques.md).
        std::string result = "[";
        for (std::size_t i = 0; i < (*deque)->elements.size(); ++i)
        {
            if (i > 0)
            {
                result += ", ";
            }
            result += toString((*deque)->elements[i]);
        }
        result += "]";
        return result;
    }
    if (const auto* queue = std::get_if<std::shared_ptr<QueueInstance>>(&value))
    {
        // Same bracket format as List/Stack/Deque above - a Queue's FIFO
        // order (front to back) is exactly as well-defined as theirs (see
        // docs/language/0038-queues.md).
        std::string result = "[";
        for (std::size_t i = 0; i < (*queue)->elements.size(); ++i)
        {
            if (i > 0)
            {
                result += ", ";
            }
            result += toString((*queue)->elements[i]);
        }
        result += "]";
        return result;
    }
    if (const auto* priorityQueue = std::get_if<std::shared_ptr<PriorityQueueInstance>>(&value))
    {
        // Same bracket format as List/Stack/Deque/Queue above - shows the
        // heap's raw internal array order, *not* sorted order (see
        // docs/language/0039-priority-queues.md), matching the LLVM
        // backend's own identical choice.
        std::string result = "[";
        for (std::size_t i = 0; i < (*priorityQueue)->elements.size(); ++i)
        {
            if (i > 0)
            {
                result += ", ";
            }
            result += toString((*priorityQueue)->elements[i]);
        }
        result += "]";
        return result;
    }
    if (const auto* linkedList = std::get_if<std::shared_ptr<LinkedListInstance>>(&value))
    {
        // Unlike List/Stack above, this deliberately falls back to a
        // count-only format, matching Map/Set's own precedent just below -
        // must match LlvmIrEmitter's top-level printer byte-for-byte (see
        // docs/language/0036-linked-lists.md), which makes the same choice.
        return "LinkedList(" + std::to_string((*linkedList)->elements.size()) + " entries)";
    }
    if (const auto* map = std::get_if<std::shared_ptr<MapInstance>>(&value))
    {
        // No iteration this phase (see docs/language/0034-maps-and-sets.md),
        // so - unlike array/slice/List above - there's no way to print
        // contents; falls back to the count field alone, mirroring the
        // LlvmIrEmitter's own top-level printer for the same reason.
        return "Map(" + std::to_string((*map)->entries.size()) + " entries)";
    }
    if (const auto* set = std::get_if<std::shared_ptr<SetInstance>>(&value))
    {
        return "Set(" + std::to_string((*set)->elements.size()) + " entries)";
    }
    if (const auto* sortedMap = std::get_if<std::shared_ptr<SortedMapInstance>>(&value))
    {
        // Count-only, matching Map/Set's own fallback - *not* full sorted
        // contents, even though SortedMapInstance's std::map could trivially
        // print them in order. Deliberate: this must match the LLVM
        // backend's own top-level printer byte-for-byte (see
        // docs/language/0040-sorted-maps.md), and that side has no
        // traversal wired up yet (no `for`-in desugaring to hang a
        // print-only walk off, same reasoning LinkedList<T>'s own
        // count-only fallback already established).
        return "SortedMap(" + std::to_string((*sortedMap)->entries.size()) + " entries)";
    }
    if (const auto* sortedSet = std::get_if<std::shared_ptr<SortedSetInstance>>(&value))
    {
        // Count-only, matching SortedMap's own fallback above and the LLVM
        // backend's identical choice (see docs/language/0041-sorted-sets.md).
        return "SortedSet(" + std::to_string((*sortedSet)->elements.size()) + " entries)";
    }
    return "()";
}

std::size_t ValueHash::operator()(const Value& value) const
{
    if (const auto* integer = std::get_if<std::int64_t>(&value))
    {
        return std::hash<std::int64_t>{}(*integer);
    }
    if (const auto* boolean = std::get_if<bool>(&value))
    {
        return std::hash<bool>{}(*boolean);
    }
    if (const auto* string = std::get_if<std::string>(&value))
    {
        return std::hash<std::string>{}(*string);
    }
    // Structural, not pointer-identity - combined via a classic djb2-style
    // accumulator (see docs/language/0034-maps-and-sets.md's generic
    // rewrite). No cycle protection needed: TypeChecker::isHashable already
    // rejects a self-referential struct as a key type before any Value of
    // that shape reaches here.
    if (const auto* instance = std::get_if<std::shared_ptr<StructInstance>>(&value))
    {
        std::size_t hash = 0;
        for (const auto& [fieldName, fieldValue] : (*instance)->fields)
        {
            hash = hash * 31 + ValueHash{}(fieldValue);
        }
        return hash;
    }
    if (const auto* array = std::get_if<std::shared_ptr<ArrayInstance>>(&value))
    {
        std::size_t hash = 0;
        for (const auto& element : (*array)->elements)
        {
            hash = hash * 31 + ValueHash{}(element);
        }
        return hash;
    }
    if (const auto* list = std::get_if<std::shared_ptr<ListInstance>>(&value))
    {
        std::size_t hash = 0;
        for (const auto& element : (*list)->elements)
        {
            hash = hash * 31 + ValueHash{}(element);
        }
        return hash;
    }
    if (const auto* stack = std::get_if<std::shared_ptr<StackInstance>>(&value))
    {
        std::size_t hash = 0;
        for (const auto& element : (*stack)->elements)
        {
            hash = hash * 31 + ValueHash{}(element);
        }
        return hash;
    }
    return 0; // SliceInstance/MapInstance/SetInstance/monostate: never valid keys
}

bool ValueEq::operator()(const Value& a, const Value& b) const
{
    if (a.index() != b.index())
    {
        return false;
    }
    if (const auto* left = std::get_if<std::int64_t>(&a))
    {
        return *left == std::get<std::int64_t>(b);
    }
    if (const auto* left = std::get_if<bool>(&a))
    {
        return *left == std::get<bool>(b);
    }
    if (const auto* left = std::get_if<std::string>(&a))
    {
        return *left == std::get<std::string>(b);
    }
    // Structural, not pointer-identity - same reasoning as ValueHash above.
    if (const auto* left = std::get_if<std::shared_ptr<StructInstance>>(&a))
    {
        const auto& right = std::get<std::shared_ptr<StructInstance>>(b);
        if ((*left)->fields.size() != right->fields.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < (*left)->fields.size(); ++i)
        {
            if (!ValueEq{}((*left)->fields[i].second, right->fields[i].second))
            {
                return false;
            }
        }
        return true;
    }
    if (const auto* left = std::get_if<std::shared_ptr<ArrayInstance>>(&a))
    {
        const auto& right = std::get<std::shared_ptr<ArrayInstance>>(b);
        if ((*left)->elements.size() != right->elements.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < (*left)->elements.size(); ++i)
        {
            if (!ValueEq{}((*left)->elements[i], right->elements[i]))
            {
                return false;
            }
        }
        return true;
    }
    if (const auto* left = std::get_if<std::shared_ptr<ListInstance>>(&a))
    {
        const auto& right = std::get<std::shared_ptr<ListInstance>>(b);
        if ((*left)->elements.size() != right->elements.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < (*left)->elements.size(); ++i)
        {
            if (!ValueEq{}((*left)->elements[i], right->elements[i]))
            {
                return false;
            }
        }
        return true;
    }
    if (const auto* left = std::get_if<std::shared_ptr<StackInstance>>(&a))
    {
        const auto& right = std::get<std::shared_ptr<StackInstance>>(b);
        if ((*left)->elements.size() != right->elements.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < (*left)->elements.size(); ++i)
        {
            if (!ValueEq{}((*left)->elements[i], right->elements[i]))
            {
                return false;
            }
        }
        return true;
    }
    return false; // SliceInstance/MapInstance/SetInstance/monostate: never valid keys
}

Environment::Environment(Environment* parent)
    : parent_(parent)
{
}

void Environment::define(const std::string& name, Value value)
{
    values_[name] = std::move(value);
}

void Environment::assign(const std::string& name, Value value)
{
    if (const auto it = values_.find(name); it != values_.end())
    {
        it->second = std::move(value);
        return;
    }
    if (parent_)
    {
        parent_->assign(name, std::move(value));
        return;
    }
    throw std::runtime_error("undefined variable: " + name);
}

Value Environment::get(const std::string& name) const
{
    if (const auto it = values_.find(name); it != values_.end())
    {
        return it->second;
    }
    if (parent_)
    {
        return parent_->get(name);
    }
    throw std::runtime_error("undefined variable: " + name);
}

const std::unordered_map<std::string, Value>& Environment::bindings() const
{
    return values_;
}

bool Environment::contains(const std::string& name) const
{
    if (values_.contains(name))
    {
        return true;
    }
    return parent_ && parent_->contains(name);
}

void Interpreter::run(const Program& program)
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

    for (const auto& item : program.items)
    {
        if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(item.get()))
        {
            execute(*assignment, globalEnv_);
        }
    }
}

void Interpreter::execute(const Stmt& stmt, Environment& env)
{
    if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(&stmt))
    {
        // Mutates an already-existing binding (in this scope or any
        // enclosing one), the same as `++`/`--` already do; only a name
        // that's genuinely new here creates a fresh local. This matters most
        // for loops: a loop body gets its own scope per iteration, and
        // `n = n + 1` needs to actually update the outer `n` the condition
        // checks, not shadow a throwaway per-iteration copy (see
        // docs/language/0028-loops.md).
        Value value = evaluate(*assignment->value, env);
        if (!assignment->forceDefine && env.contains(assignment->name))
        {
            env.assign(assignment->name, std::move(value));
        }
        else
        {
            env.define(assignment->name, std::move(value));
        }
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt))
    {
        Value value =
            returnStmt->value ? evaluate(*returnStmt->value, env) : Value{std::monostate{}};
        throw ReturnSignal{std::move(value)};
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt))
    {
        evaluate(*exprStmt->expr, env);
        return;
    }

    if (const auto* fieldAssign = dynamic_cast<const FieldAssignStmt*>(&stmt))
    {
        auto objectValue = evaluate(*fieldAssign->object, env);
        const auto* instance = std::get_if<std::shared_ptr<StructInstance>>(&objectValue);
        if (!instance)
        {
            throw std::runtime_error("field assignment on a non-struct value");
        }
        auto newValue = evaluate(*fieldAssign->value, env);
        for (auto& [fieldName, fieldValue] : (*instance)->fields)
        {
            if (fieldName == fieldAssign->field)
            {
                fieldValue = std::move(newValue);
                return;
            }
        }
        throw std::runtime_error("no such field: " + fieldAssign->field);
    }

    if (const auto* indexAssign = dynamic_cast<const IndexAssignStmt*>(&stmt))
    {
        auto objectValue = evaluate(*indexAssign->object, env);
        auto indexable = asIndexable(objectValue);
        if (!indexable)
        {
            throw std::runtime_error("indexed assignment on a non-array/slice value");
        }
        const std::int64_t indexValue = asInt(evaluate(*indexAssign->index, env));
        if (indexValue < 0 || static_cast<std::size_t>(indexValue) >= indexable->length)
        {
            throw std::runtime_error("array index " + std::to_string(indexValue) +
                                     " out of bounds for array of size " +
                                     std::to_string(indexable->length));
        }
        (*indexable->elements)[static_cast<std::size_t>(indexValue)] =
            evaluate(*indexAssign->value, env);
        return;
    }

    if (const auto* incDec = dynamic_cast<const IncDecStmt*>(&stmt))
    {
        const std::int64_t delta = incDec->increment ? 1 : -1;

        if (const auto* name = dynamic_cast<const NameExpr*>(incDec->target.get()))
        {
            env.assign(name->name, asInt(env.get(name->name)) + delta);
            return;
        }

        if (const auto* field = dynamic_cast<const FieldExpr*>(incDec->target.get()))
        {
            auto objectValue = evaluate(*field->object, env);
            const auto* instance = std::get_if<std::shared_ptr<StructInstance>>(&objectValue);
            if (!instance)
            {
                throw std::runtime_error("field increment/decrement on a non-struct value");
            }
            for (auto& [fieldName, fieldValue] : (*instance)->fields)
            {
                if (fieldName == field->field)
                {
                    fieldValue = asInt(fieldValue) + delta;
                    return;
                }
            }
            throw std::runtime_error("no such field: " + field->field);
        }

        throw std::runtime_error("invalid increment/decrement target");
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt))
    {
        const auto& block = static_cast<const BlockExpr&>(*whileStmt->body);
        while (asBool(evaluate(*whileStmt->condition, env)))
        {
            try
            {
                // Fresh scope per iteration (matches BlockExpr's own
                // per-evaluation child scope); the trailing result, if any,
                // is evaluated for side effects only and discarded - `while`
                // never produces a value (docs/language/0028-loops.md).
                Environment bodyEnv(&env);
                for (const auto& bodyStmt : block.statements)
                {
                    execute(*bodyStmt, bodyEnv);
                }
                if (block.result)
                {
                    evaluate(*block.result, bodyEnv);
                }
            }
            catch (ContinueSignal&)
            {
                continue;
            }
            catch (BreakSignal&)
            {
                break;
            }
        }
        return;
    }

    if (const auto* breakStmt = dynamic_cast<const BreakStmt*>(&stmt))
    {
        Value value = breakStmt->value ? evaluate(*breakStmt->value, env) : Value{std::monostate{}};
        throw BreakSignal{std::move(value)};
    }

    if (dynamic_cast<const ContinueStmt*>(&stmt))
    {
        throw ContinueSignal{};
    }

    throw std::runtime_error("unsupported statement");
}

Value Interpreter::evaluate(const Expr& expr, Environment& env)
{
    if (const auto* integer = dynamic_cast<const IntegerExpr*>(&expr))
    {
        return integer->value;
    }

    if (const auto* boolean = dynamic_cast<const BoolExpr*>(&expr))
    {
        return boolean->value;
    }

    if (const auto* string = dynamic_cast<const StringExpr*>(&expr))
    {
        return string->value;
    }

    if (const auto* character = dynamic_cast<const CharExpr*>(&expr))
    {
        return static_cast<char32_t>(character->codepoint);
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        return env.get(name->name);
    }

    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
    {
        if (asBool(evaluate(*ifExpr->condition, env)))
        {
            return evaluate(*ifExpr->thenBranch, env);
        }
        return evaluate(*ifExpr->elseBranch, env);
    }

    if (const auto* loopExpr = dynamic_cast<const LoopExpr*>(&expr))
    {
        const auto& block = static_cast<const BlockExpr&>(*loopExpr->body);
        while (true)
        {
            try
            {
                // Same "discard the trailing result, only an explicit exit
                // produces the real value" shape as `while` above - a `loop`
                // never falls off the end of its body normally, it only ever
                // exits via `break` (or diverges).
                Environment bodyEnv(&env);
                for (const auto& stmt : block.statements)
                {
                    execute(*stmt, bodyEnv);
                }
                if (block.result)
                {
                    evaluate(*block.result, bodyEnv);
                }
            }
            catch (ContinueSignal&)
            {
                continue;
            }
            catch (BreakSignal& signal)
            {
                return std::move(signal.value);
            }
        }
    }

    if (const auto* block = dynamic_cast<const BlockExpr*>(&expr))
    {
        Environment blockEnv(&env);
        for (const auto& statement : block->statements)
        {
            execute(*statement, blockEnv);
        }
        if (block->result)
        {
            return evaluate(*block->result, blockEnv);
        }
        return Value{std::monostate{}};
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        const auto it = functions_.find(call->callee);
        if (it == functions_.end())
        {
            throw std::runtime_error("undefined function: " + call->callee);
        }

        std::vector<Value> args;
        args.reserve(call->arguments.size());
        for (std::size_t i = 0; i < call->arguments.size(); ++i)
        {
            Value argValue = evaluate(*call->arguments[i], env);
            // Implicit array -> slice conversion at the call boundary - the
            // whole point of slice<T> (docs/language/0032-slices.md). An
            // argument that's already a slice (forwarding to another slice
            // parameter) passes through unchanged - only get_if'ing for
            // ArrayInstance below means a SliceInstance value is untouched.
            if (i < it->second->params.size() && it->second->params[i].type.starts_with("slice<"))
            {
                if (const auto* array = std::get_if<std::shared_ptr<ArrayInstance>>(&argValue))
                {
                    argValue = std::make_shared<SliceInstance>(
                        SliceInstance{*array, (*array)->elements.size()});
                }
            }
            // Implicit String -> str conversion at the call boundary (see
            // docs/language/0042-string.md and TypeChecker::isStrCoercible's
            // identical rule) - mirrors the array->slice conversion above
            // for a different pair of types. Critically a real *copy* of
            // the current content, not just re-wrapping the same
            // shared_ptr<StringInstance>: `str` is an immutable value, so
            // this must sever the connection to the original String the
            // same way the LLVM backend's own resolveStrPtr does by
            // capturing the data pointer at the moment of the call - an
            // alias here would let a later `.append()` on the source
            // String retroactively change an already-passed str, which
            // the compiled backend can't do (its own str stays pointing at
            // the pre-append buffer, since append reallocates rather than
            // mutating in place - see emitStringAppend).
            if (i < it->second->params.size() && it->second->params[i].type == "str")
            {
                if (const auto* ownedString =
                        std::get_if<std::shared_ptr<StringInstance>>(&argValue))
                {
                    argValue = (*ownedString)->data;
                }
            }
            args.push_back(std::move(argValue));
        }

        return callFunction(*it->second, std::move(args));
    }

    if (const auto* methodCall = dynamic_cast<const MethodCallExpr*>(&expr))
    {
        auto objectValue = evaluate(*methodCall->object, env);

        if (const auto* list = std::get_if<std::shared_ptr<ListInstance>>(&objectValue))
        {
            if (methodCall->method == "push")
            {
                (*list)->elements.push_back(evaluate(*methodCall->arguments.front(), env));
                return Value{std::monostate{}};
            }

            if (methodCall->method == "pop")
            {
                if ((*list)->elements.empty())
                {
                    throw std::runtime_error("pop on an empty List");
                }
                Value popped = std::move((*list)->elements.back());
                (*list)->elements.pop_back();
                return popped;
            }

            throw std::runtime_error("no such method: " + methodCall->method);
        }

        // Stack<T> (see docs/language/0035-stacks.md) - push/pop mirror
        // List<T>'s own exactly; peek reads the top without removing
        // (throws on empty too, for the same "interpreter checks, compiled
        // code doesn't" reason pop already does).
        if (const auto* stack = std::get_if<std::shared_ptr<StackInstance>>(&objectValue))
        {
            if (methodCall->method == "push")
            {
                (*stack)->elements.push_back(evaluate(*methodCall->arguments.front(), env));
                return Value{std::monostate{}};
            }

            if (methodCall->method == "pop")
            {
                if ((*stack)->elements.empty())
                {
                    throw std::runtime_error("pop on an empty Stack");
                }
                Value popped = std::move((*stack)->elements.back());
                (*stack)->elements.pop_back();
                return popped;
            }

            if (methodCall->method == "peek")
            {
                if ((*stack)->elements.empty())
                {
                    throw std::runtime_error("peek on an empty Stack");
                }
                return (*stack)->elements.back();
            }

            throw std::runtime_error("no such method: " + methodCall->method);
        }

        // LinkedList<T> (see docs/language/0036-linked-lists.md) -
        // push_front/push_back/pop_front/pop_back are std::deque's own
        // push_front/push_back/pop_front/pop_back directly; pop_front/
        // pop_back throw on empty, same reason Stack<T>.pop()/.peek() do.
        if (const auto* linkedList = std::get_if<std::shared_ptr<LinkedListInstance>>(&objectValue))
        {
            if (methodCall->method == "push_front")
            {
                (*linkedList)->elements.push_front(evaluate(*methodCall->arguments.front(), env));
                return Value{std::monostate{}};
            }

            if (methodCall->method == "push_back")
            {
                (*linkedList)->elements.push_back(evaluate(*methodCall->arguments.front(), env));
                return Value{std::monostate{}};
            }

            if (methodCall->method == "pop_front")
            {
                if ((*linkedList)->elements.empty())
                {
                    throw std::runtime_error("pop_front on an empty LinkedList");
                }
                Value popped = std::move((*linkedList)->elements.front());
                (*linkedList)->elements.pop_front();
                return popped;
            }

            if (methodCall->method == "pop_back")
            {
                if ((*linkedList)->elements.empty())
                {
                    throw std::runtime_error("pop_back on an empty LinkedList");
                }
                Value popped = std::move((*linkedList)->elements.back());
                (*linkedList)->elements.pop_back();
                return popped;
            }

            throw std::runtime_error("no such method: " + methodCall->method);
        }

        // Deque<T> (see docs/language/0037-deques.md) - push_front/pop_front
        // use std::vector's own insert/erase at begin() (O(n) here, purely
        // an interpreter representation choice - the LLVM backend's actual
        // representation is genuinely O(1)); push_back/pop_back are
        // std::vector's own push_back/pop_back directly. pop_front/pop_back
        // throw on empty, same reason LinkedList<T>.pop_front()/.pop_back()
        // do. `[i]`/`[i]=`/.length are handled generically via asIndexable,
        // not here.
        if (const auto* deque = std::get_if<std::shared_ptr<DequeInstance>>(&objectValue))
        {
            if (methodCall->method == "push_front")
            {
                (*deque)->elements.insert((*deque)->elements.begin(),
                                          evaluate(*methodCall->arguments.front(), env));
                return Value{std::monostate{}};
            }

            if (methodCall->method == "push_back")
            {
                (*deque)->elements.push_back(evaluate(*methodCall->arguments.front(), env));
                return Value{std::monostate{}};
            }

            if (methodCall->method == "pop_front")
            {
                if ((*deque)->elements.empty())
                {
                    throw std::runtime_error("pop_front on an empty Deque");
                }
                Value popped = std::move((*deque)->elements.front());
                (*deque)->elements.erase((*deque)->elements.begin());
                return popped;
            }

            if (methodCall->method == "pop_back")
            {
                if ((*deque)->elements.empty())
                {
                    throw std::runtime_error("pop_back on an empty Deque");
                }
                Value popped = std::move((*deque)->elements.back());
                (*deque)->elements.pop_back();
                return popped;
            }

            throw std::runtime_error("no such method: " + methodCall->method);
        }

        // Queue<T> (see docs/language/0038-queues.md) - `enqueue` is
        // std::deque's own push_back; `dequeue` is std::deque's own
        // pop_front, matching classic FIFO order. `dequeue` throws on empty,
        // same reason Deque<T>.pop_front()/.pop_back() do.
        if (const auto* queue = std::get_if<std::shared_ptr<QueueInstance>>(&objectValue))
        {
            if (methodCall->method == "enqueue")
            {
                (*queue)->elements.push_back(evaluate(*methodCall->arguments.front(), env));
                return Value{std::monostate{}};
            }

            if (methodCall->method == "dequeue")
            {
                if ((*queue)->elements.empty())
                {
                    throw std::runtime_error("dequeue on an empty Queue");
                }
                Value popped = std::move((*queue)->elements.front());
                (*queue)->elements.pop_front();
                return popped;
            }

            throw std::runtime_error("no such method: " + methodCall->method);
        }

        // PriorityQueue<T> (see docs/language/0039-priority-queues.md) - a
        // real binary heap: push/pop run std::push_heap/std::pop_heap with
        // priorityQueueLess (ascending, smallest-first - see that
        // comparator's own comment above), the same algorithm the LLVM
        // backend hand-rolls, not a renamed vector operation like every
        // earlier collection's push/pop here. pop()/peek() throw on empty,
        // same reason Stack<T>.pop()/.peek() do.
        if (const auto* priorityQueue =
                std::get_if<std::shared_ptr<PriorityQueueInstance>>(&objectValue))
        {
            if (methodCall->method == "push")
            {
                (*priorityQueue)->elements.push_back(evaluate(*methodCall->arguments.front(), env));
                std::push_heap((*priorityQueue)->elements.begin(),
                               (*priorityQueue)->elements.end(),
                               priorityQueueLess);
                return Value{std::monostate{}};
            }

            if (methodCall->method == "pop")
            {
                if ((*priorityQueue)->elements.empty())
                {
                    throw std::runtime_error("pop on an empty PriorityQueue");
                }
                std::pop_heap((*priorityQueue)->elements.begin(),
                              (*priorityQueue)->elements.end(),
                              priorityQueueLess);
                Value popped = std::move((*priorityQueue)->elements.back());
                (*priorityQueue)->elements.pop_back();
                return popped;
            }

            if (methodCall->method == "peek")
            {
                if ((*priorityQueue)->elements.empty())
                {
                    throw std::runtime_error("peek on an empty PriorityQueue");
                }
                return (*priorityQueue)->elements.front();
            }

            throw std::runtime_error("no such method: " + methodCall->method);
        }

        // Map<i32,i32>/Set<i32> (see docs/language/0034-maps-and-sets.md).
        // `.get` on a missing key throws here - unlike compiled code, which
        // returns an unspecified sentinel - matching the established
        // "interpreter checks, compiled code doesn't" split already used for
        // division by zero and array/slice/List indexing.
        if (const auto* map = std::get_if<std::shared_ptr<MapInstance>>(&objectValue))
        {
            if (methodCall->method == "set")
            {
                Value key = evaluate(*methodCall->arguments[0], env);
                Value value = evaluate(*methodCall->arguments[1], env);
                (*map)->entries[std::move(key)] = std::move(value);
                return Value{std::monostate{}};
            }
            if (methodCall->method == "get")
            {
                const Value key = evaluate(*methodCall->arguments.front(), env);
                const auto it = (*map)->entries.find(key);
                if (it == (*map)->entries.end())
                {
                    throw std::runtime_error("Map.get on a missing key");
                }
                return it->second;
            }
            if (methodCall->method == "contains")
            {
                const Value key = evaluate(*methodCall->arguments.front(), env);
                return (*map)->entries.contains(key);
            }
            if (methodCall->method == "remove")
            {
                const Value key = evaluate(*methodCall->arguments.front(), env);
                (*map)->entries.erase(key);
                return Value{std::monostate{}};
            }
            throw std::runtime_error("no such method: " + methodCall->method);
        }

        if (const auto* set = std::get_if<std::shared_ptr<SetInstance>>(&objectValue))
        {
            if (methodCall->method == "add")
            {
                Value value = evaluate(*methodCall->arguments.front(), env);
                (*set)->elements.insert(std::move(value));
                return Value{std::monostate{}};
            }
            if (methodCall->method == "contains")
            {
                const Value value = evaluate(*methodCall->arguments.front(), env);
                return (*set)->elements.contains(value);
            }
            if (methodCall->method == "remove")
            {
                const Value value = evaluate(*methodCall->arguments.front(), env);
                (*set)->elements.erase(value);
                return Value{std::monostate{}};
            }
            throw std::runtime_error("no such method: " + methodCall->method);
        }

        // SortedMap<K,V> (see docs/language/0040-sorted-maps.md) - set/get/
        // contains/remove are Map<i32,i32>'s own shape exactly; std::map's
        // ordinary operations already give the right semantics (see
        // SortedMapInstance's own doc comment). `.get` on a missing key
        // throws here, same reasoning as Map<K,V>.get() above.
        if (const auto* sortedMap = std::get_if<std::shared_ptr<SortedMapInstance>>(&objectValue))
        {
            if (methodCall->method == "set")
            {
                const std::int64_t key = asInt(evaluate(*methodCall->arguments[0], env));
                Value value = evaluate(*methodCall->arguments[1], env);
                (*sortedMap)->entries[key] = std::move(value);
                return Value{std::monostate{}};
            }
            if (methodCall->method == "get")
            {
                const std::int64_t key = asInt(evaluate(*methodCall->arguments.front(), env));
                const auto it = (*sortedMap)->entries.find(key);
                if (it == (*sortedMap)->entries.end())
                {
                    throw std::runtime_error("SortedMap.get on a missing key");
                }
                return it->second;
            }
            if (methodCall->method == "contains")
            {
                const std::int64_t key = asInt(evaluate(*methodCall->arguments.front(), env));
                return (*sortedMap)->entries.contains(key);
            }
            if (methodCall->method == "remove")
            {
                const std::int64_t key = asInt(evaluate(*methodCall->arguments.front(), env));
                (*sortedMap)->entries.erase(key);
                return Value{std::monostate{}};
            }
            throw std::runtime_error("no such method: " + methodCall->method);
        }

        // SortedSet<T> (see docs/language/0041-sorted-sets.md) - add/
        // contains/remove are Set<T>'s own shape exactly; std::set's
        // ordinary operations already give the right semantics (see
        // SortedSetInstance's own doc comment).
        if (const auto* sortedSet = std::get_if<std::shared_ptr<SortedSetInstance>>(&objectValue))
        {
            if (methodCall->method == "add")
            {
                const std::int64_t value = asInt(evaluate(*methodCall->arguments.front(), env));
                (*sortedSet)->elements.insert(value);
                return Value{std::monostate{}};
            }
            if (methodCall->method == "contains")
            {
                const std::int64_t value = asInt(evaluate(*methodCall->arguments.front(), env));
                return (*sortedSet)->elements.contains(value);
            }
            if (methodCall->method == "remove")
            {
                const std::int64_t value = asInt(evaluate(*methodCall->arguments.front(), env));
                (*sortedSet)->elements.erase(value);
                return Value{std::monostate{}};
            }
            throw std::runtime_error("no such method: " + methodCall->method);
        }

        // String (see docs/language/0042-string.md) - `append` accepts
        // anything str-coercible (asStrContent resolves either shape) and
        // mutates `data` in place, the same reference-semantics model
        // every other collection's own mutating method here already
        // follows.
        if (const auto* string = std::get_if<std::shared_ptr<StringInstance>>(&objectValue))
        {
            if (methodCall->method == "append")
            {
                (*string)->data += asStrContent(evaluate(*methodCall->arguments.front(), env));
                return Value{std::monostate{}};
            }
            throw std::runtime_error("no such method: " + methodCall->method);
        }

        // Buffer (see docs/language/0043-buffer.md) - a mutable,
        // amortized-growth text-construction type. std::string already
        // does its own amortized growth internally, so
        // append/append_line/clear/reserve/finish all just ride its
        // existing `+=`/`clear`/`reserve` machinery directly - the real
        // conditional-growth branch the LLVM backend hand-emits
        // (emitBufferAppend/ensureBufferCapacity) has no interpreter-side
        // equivalent to write, since std::string already provides it.
        if (const auto* buffer = std::get_if<std::shared_ptr<BufferInstance>>(&objectValue))
        {
            if (methodCall->method == "append")
            {
                (*buffer)->data += asStrContent(evaluate(*methodCall->arguments.front(), env));
                return Value{std::monostate{}};
            }
            if (methodCall->method == "append_line")
            {
                (*buffer)->data += asStrContent(evaluate(*methodCall->arguments.front(), env));
                (*buffer)->data += '\n';
                return Value{std::monostate{}};
            }
            if (methodCall->method == "clear")
            {
                (*buffer)->data.clear();
                return Value{std::monostate{}};
            }
            if (methodCall->method == "reserve")
            {
                const std::int64_t capacity = asInt(evaluate(*methodCall->arguments.front(), env));
                (*buffer)->data.reserve(static_cast<std::size_t>(capacity));
                return Value{std::monostate{}};
            }
            if (methodCall->method == "finish")
            {
                // Genuine ownership transfer, mirroring emitBufferFinish's
                // own zero-copy steal + reset - std::move empties `data`
                // in place (guaranteed valid-but-unspecified by the
                // standard; immediately reset to "" below so the buffer
                // stays in the exact same fresh, empty state
                // BufferNewExpr's own construction produces, never left
                // in a merely "unspecified" state).
                auto result =
                    std::make_shared<StringInstance>(StringInstance{std::move((*buffer)->data)});
                (*buffer)->data = std::string();
                return Value{result};
            }
            throw std::runtime_error("no such method: " + methodCall->method);
        }

        throw std::runtime_error("no such method '" + methodCall->method + "' on this value");
    }

    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        auto objectValue = evaluate(*field->object, env);

        if (auto indexable = asIndexable(objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>(indexable->length);
            }
            throw std::runtime_error("no such field: " + field->field);
        }

        // Map<i32,i32>/Set<i32> aren't indexable (unordered - no `[i]`), so
        // this is a standalone case rather than folded into asIndexable
        // above (see docs/language/0034-maps-and-sets.md).
        if (const auto* map = std::get_if<std::shared_ptr<MapInstance>>(&objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>((*map)->entries.size());
            }
            throw std::runtime_error("no such field: " + field->field);
        }
        if (const auto* set = std::get_if<std::shared_ptr<SetInstance>>(&objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>((*set)->elements.size());
            }
            throw std::runtime_error("no such field: " + field->field);
        }

        // SortedMap<K,V> deliberately isn't indexable either - no `[key]`
        // syntax and no `for`-in iteration this phase (see
        // docs/language/0040-sorted-maps.md).
        if (const auto* sortedMap = std::get_if<std::shared_ptr<SortedMapInstance>>(&objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>((*sortedMap)->entries.size());
            }
            throw std::runtime_error("no such field: " + field->field);
        }

        // SortedSet<T> deliberately isn't indexable either - same reasoning
        // as SortedMap<K,V> above (see docs/language/0041-sorted-sets.md).
        if (const auto* sortedSet = std::get_if<std::shared_ptr<SortedSetInstance>>(&objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>((*sortedSet)->elements.size());
            }
            throw std::runtime_error("no such field: " + field->field);
        }

        // String isn't indexable either this phase - slicing is
        // deliberately out of scope (see docs/language/0042-string.md).
        // Unlike the LLVM backend (where `.length` rides isListType's own
        // existing structural check for free, since String's header is
        // byte-for-byte List<i8>'s shape), the interpreter's dispatch is by
        // real C++ type, not by shape, so this needs its own standalone
        // case just like every other non-List-backed collection here does.
        if (const auto* string = std::get_if<std::shared_ptr<StringInstance>>(&objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>((*string)->data.size());
            }
            throw std::runtime_error("no such field: " + field->field);
        }

        // Buffer isn't indexable either, same reasoning as String above
        // (see docs/language/0043-buffer.md). `.capacity` rides
        // std::string's own `.capacity()` directly - its exact numeric
        // value is expected to diverge from the LLVM backend's own
        // explicit doubling formula (see BufferInstance's own comment
        // above); `.length` (content size) is not expected to diverge and
        // must match exactly.
        if (const auto* buffer = std::get_if<std::shared_ptr<BufferInstance>>(&objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>((*buffer)->data.size());
            }
            if (field->field == "capacity")
            {
                return static_cast<std::int64_t>((*buffer)->data.capacity());
            }
            throw std::runtime_error("no such field: " + field->field);
        }

        // Stack<T> isn't indexable either (LIFO access only, via
        // push/pop/peek, no `[i]`), so this is also a standalone case
        // rather than folded into asIndexable above (see
        // docs/language/0035-stacks.md).
        if (const auto* stack = std::get_if<std::shared_ptr<StackInstance>>(&objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>((*stack)->elements.size());
            }
            throw std::runtime_error("no such field: " + field->field);
        }

        // LinkedList<T> isn't indexable either (node-based, front/back access
        // only, no `[i]`), so this is also a standalone case (see
        // docs/language/0036-linked-lists.md).
        if (const auto* linkedList = std::get_if<std::shared_ptr<LinkedListInstance>>(&objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>((*linkedList)->elements.size());
            }
            throw std::runtime_error("no such field: " + field->field);
        }

        // Queue<T> deliberately isn't indexable either, even though it's
        // backed by Deque<T>'s own (indexable) representation -
        // "communicate intent" (see docs/language/0038-queues.md).
        if (const auto* queue = std::get_if<std::shared_ptr<QueueInstance>>(&objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>((*queue)->elements.size());
            }
            throw std::runtime_error("no such field: " + field->field);
        }

        // PriorityQueue<T> deliberately isn't indexable either - a heap's
        // internal array order isn't even a meaningful order to expose (see
        // docs/language/0039-priority-queues.md).
        if (const auto* priorityQueue =
                std::get_if<std::shared_ptr<PriorityQueueInstance>>(&objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>((*priorityQueue)->elements.size());
            }
            throw std::runtime_error("no such field: " + field->field);
        }

        const auto* instance = std::get_if<std::shared_ptr<StructInstance>>(&objectValue);
        if (!instance)
        {
            throw std::runtime_error("field access on a non-struct value");
        }
        for (const auto& [fieldName, fieldValue] : (*instance)->fields)
        {
            if (fieldName == field->field)
            {
                return fieldValue;
            }
        }
        throw std::runtime_error("no such field: " + field->field);
    }

    if (const auto* arrayLiteral = dynamic_cast<const ArrayLiteralExpr*>(&expr))
    {
        auto instance = std::make_shared<ArrayInstance>();
        instance->elements.reserve(arrayLiteral->elements.size());
        for (const auto& element : arrayLiteral->elements)
        {
            instance->elements.push_back(evaluate(*element, env));
        }
        return instance;
    }

    if (dynamic_cast<const ListNewExpr*>(&expr))
    {
        return std::make_shared<ListInstance>();
    }

    if (dynamic_cast<const LinkedListNewExpr*>(&expr))
    {
        return std::make_shared<LinkedListInstance>();
    }

    if (dynamic_cast<const DequeNewExpr*>(&expr))
    {
        return std::make_shared<DequeInstance>();
    }

    if (dynamic_cast<const QueueNewExpr*>(&expr))
    {
        return std::make_shared<QueueInstance>();
    }

    if (dynamic_cast<const PriorityQueueNewExpr*>(&expr))
    {
        return std::make_shared<PriorityQueueInstance>();
    }

    if (dynamic_cast<const StackNewExpr*>(&expr))
    {
        return std::make_shared<StackInstance>();
    }

    if (dynamic_cast<const MapNewExpr*>(&expr))
    {
        return std::make_shared<MapInstance>();
    }

    if (dynamic_cast<const SetNewExpr*>(&expr))
    {
        return std::make_shared<SetInstance>();
    }

    if (dynamic_cast<const SortedMapNewExpr*>(&expr))
    {
        return std::make_shared<SortedMapInstance>();
    }

    if (dynamic_cast<const SortedSetNewExpr*>(&expr))
    {
        return std::make_shared<SortedSetInstance>();
    }

    if (const auto* stringNew = dynamic_cast<const StringNewExpr*>(&expr))
    {
        return std::make_shared<StringInstance>(
            StringInstance{asStrContent(evaluate(*stringNew->text, env))});
    }

    if (dynamic_cast<const BufferNewExpr*>(&expr))
    {
        return std::make_shared<BufferInstance>();
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expr))
    {
        auto objectValue = evaluate(*index->object, env);
        auto indexable = asIndexable(objectValue);
        if (!indexable)
        {
            throw std::runtime_error("indexing a non-array/slice value");
        }
        const std::int64_t indexValue = asInt(evaluate(*index->index, env));
        if (indexValue < 0 || static_cast<std::size_t>(indexValue) >= indexable->length)
        {
            throw std::runtime_error("array index " + std::to_string(indexValue) +
                                     " out of bounds for array of size " +
                                     std::to_string(indexable->length));
        }
        return (*indexable->elements)[static_cast<std::size_t>(indexValue)];
    }

    if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expr))
    {
        const auto it = structs_.find(literal->typeName);
        if (it == structs_.end())
        {
            throw std::runtime_error("undefined struct type: " + literal->typeName);
        }

        auto instance = std::make_shared<StructInstance>();
        instance->typeName = literal->typeName;

        for (const auto& declaredField : it->second->fields)
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
                throw std::runtime_error("missing field in struct literal: " + declaredField.name);
            }
            instance->fields.emplace_back(declaredField.name, evaluate(*initializer, env));
        }

        return instance;
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr))
    {
        const auto left = evaluate(*binary->left, env);
        const auto right = evaluate(*binary->right, env);

        switch (binary->op)
        {
            case TokenKind::Plus: return asInt(left) + asInt(right);
            case TokenKind::Minus: return asInt(left) - asInt(right);
            case TokenKind::Star: return asInt(left) * asInt(right);
            case TokenKind::Slash:
                if (asInt(right) == 0)
                {
                    throw std::runtime_error("division by zero");
                }
                return asInt(left) / asInt(right);
            case TokenKind::EqualEqual: return left == right;
            case TokenKind::BangEqual: return !(left == right);
            case TokenKind::Less: return asInt(left) < asInt(right);
            case TokenKind::LessEqual: return asInt(left) <= asInt(right);
            case TokenKind::Greater: return asInt(left) > asInt(right);
            case TokenKind::GreaterEqual: return asInt(left) >= asInt(right);
            default: throw std::runtime_error("unsupported operator");
        }
    }

    throw std::runtime_error("unsupported expression");
}

Value Interpreter::callFunction(const FunctionDecl& decl, std::vector<Value> args)
{
    if (args.size() != decl.params.size())
    {
        throw std::runtime_error("wrong number of arguments to " + decl.name);
    }

    Environment env; // no parent: no closures over globals or other calls' locals
    for (std::size_t i = 0; i < decl.params.size(); ++i)
    {
        env.define(decl.params[i].name, std::move(args[i]));
    }

    // Deliberately not the generic BlockExpr evaluation (which would return
    // the trailing result expression's value) - functions require an
    // explicit `return` for anything but unit (docs/language/0023), so a
    // leftover trailing expression here is only ever a discarded value; its
    // result must never leak out as the function's return.
    const auto& body = static_cast<const BlockExpr&>(*decl.body);
    try
    {
        Environment bodyEnv(&env);
        for (const auto& statement : body.statements)
        {
            execute(*statement, bodyEnv);
        }
        if (body.result)
        {
            evaluate(*body.result, bodyEnv);
        }
    }
    catch (ReturnSignal& signal)
    {
        return std::move(signal.value);
    }
    return Value{std::monostate{}};
}

const std::unordered_map<std::string, Value>& Interpreter::variables() const
{
    return globalEnv_.bindings();
}
