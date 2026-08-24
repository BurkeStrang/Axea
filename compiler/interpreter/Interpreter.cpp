#include "interpreter/Interpreter.hpp"

#include "sema/FormatSpec.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace
{
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

    // Ascending min-heap comparator for PriorityQueueInstance (see
    // docs/language/0039-priority-queues.md) - reuses ValueLess (declared
    // in Interpreter.hpp, defined further down this file), the same
    // TypeChecker::isOrderableKind-scoped comparator SortedMap<K,V>/
    // SortedSet<T> use, so str elements order correctly here too, not just
    // i32/char. std::push_heap/std::pop_heap are max-heaps by their own
    // default (comparator returns true when the *first* argument is lower
    // priority); reversing the comparison (`b < a` instead of `a < b`)
    // makes the smallest element the one that keeps rising to the front,
    // exactly the "ascending, smallest-first" choice this phase makes at
    // the LLVM-backend level too.
    bool priorityQueueLess(const Value& a, const Value& b)
    {
        return ValueLess{}(b, a);
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

    // True for exactly the two Value alternatives asStrContent above
    // accepts - used by BinaryExpr's own EqualEqual/BangEqual case to
    // route str/String through real content equality (asStrContent),
    // instead of std::variant's own defaulted `==`, which would compare a
    // String's `shared_ptr<StringInstance>` by pointer identity rather
    // than the bytes it owns (a bare `str`'s own `std::string` alternative
    // already compares by value, so this only actually changes String's
    // behavior - see docs/language/0042-string.md).
    bool isStrCoercibleValue(const Value& value)
    {
        return std::holds_alternative<std::string>(value) ||
               std::holds_alternative<std::shared_ptr<StringInstance>>(value);
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

    // Mirrors ensureBufferCapacity's own exact doubling-growth rule bit
    // for bit (see docs/language/0043-buffer.md), so `Buffer.capacity`
    // agrees between the interpreter and the compiled backend - not
    // `std::string::capacity()`, which reports libstdc++'s own
    // implementation-defined growth/SSO threshold, a genuinely different
    // (and, until this fix, silently wrong) quantity.
    void growBufferCapacity(BufferInstance& buffer, std::int64_t needed)
    {
        if (needed > buffer.capacity)
        {
            const std::int64_t doubled = buffer.capacity * 2;
            buffer.capacity = needed > doubled ? needed : doubled;
        }
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

    // Counts Unicode scalar values (codepoints) in a UTF-8 byte sequence -
    // the standard "count non-continuation bytes" algorithm: every byte
    // whose top two bits aren't `10` starts a new codepoint (see
    // docs/language/0047-unicode.md). Mirrors
    // LlvmIrEmitter::registerUtf8CountRuntime's own @axea.utf8.count
    // exactly (same byte-mask test), just as plain C++ instead of
    // hand-emitted LLVM IR - the interpreter's own ".length" (str/String/
    // Buffer) now means this, not `.size()`, which moved to `.bytes`.
    std::size_t countCodepoints(const std::string& text)
    {
        std::size_t count = 0;
        for (const unsigned char byte : text)
        {
            if ((byte & 0xC0) != 0x80)
            {
                ++count;
            }
        }
        return count;
    }

    // Decodes the codepoint at Unicode codepoint index `index` in a UTF-8
    // byte sequence (`s[i]` - see docs/language/0047-unicode.md) - walks
    // byte-by-byte, skipping continuation bytes the same way
    // countCodepoints does above, but stops at the target codepoint and
    // decodes it using the identical bit formulas
    // Parser::decodeCharLiteral uses for a char literal (same lead-byte-
    // length detection, same continuation-byte accumulation via `(cp << 6)
    // | (byte & 0x3F)`) - just over runtime string data instead of a
    // compile-time literal, and with no overlong/surrogate/range
    // validation (that's a literal-only concern; real string data is
    // presumed already-valid UTF-8). Throws if `index` is out of range,
    // matching every other collection's own "interpreter checks, compiled
    // code doesn't" precedent (see docs/language/0031-arrays.md).
    char32_t codepointAt(const std::string& text, std::int64_t index)
    {
        if (index < 0)
        {
            throw std::runtime_error("string index out of range");
        }
        std::size_t bytePos = 0;
        std::int64_t codepointIndex = 0;
        while (bytePos < text.size())
        {
            const unsigned char lead = static_cast<unsigned char>(text[bytePos]);
            std::size_t length = 0;
            std::uint32_t codepoint = 0;
            if ((lead & 0x80) == 0x00)
            {
                length = 1;
                codepoint = lead;
            }
            else if ((lead & 0xE0) == 0xC0)
            {
                length = 2;
                codepoint = lead & 0x1F;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                length = 3;
                codepoint = lead & 0x0F;
            }
            else
            {
                length = 4;
                codepoint = lead & 0x07;
            }

            if (codepointIndex == index)
            {
                for (std::size_t i = 1; i < length; ++i)
                {
                    const unsigned char cont = static_cast<unsigned char>(text[bytePos + i]);
                    codepoint = (codepoint << 6) | (cont & 0x3F);
                }
                return static_cast<char32_t>(codepoint);
            }

            bytePos += length;
            ++codepointIndex;
        }
        throw std::runtime_error("string index out of range");
    }

    // `{expr:spec}` (see docs/language/0055-numeric-format-specs.md) -
    // real value formatting, mirroring the compiled backend's own
    // sprintf-based/hand-rolled-binary logic (registerFormatRuntime)
    // exactly, verified byte-for-byte via the usual diff-against-
    // compiled-output discipline. A radix conversion (x/X/b/o) always
    // operates on the full 64-bit bit pattern, regardless of whether the
    // piece's own checked type was i32 or i64 - TypeChecker::checkExpr's
    // own result is never persisted onto the AST anywhere in this
    // codebase (every pass re-derives what it needs independently - see
    // IrGenerator's own isListType/isSetType etc.), and the interpreter's
    // `Value` itself has no i32-vs-i64 distinction at runtime either (the
    // established "TypeChecker distinguishes width, the interpreter
    // doesn't" convention - see docs/language/0051-numeric-widening.md).
    // Threading that distinction through just for this one case wasn't
    // worth it: a documented, deliberate simplification, not a bug -
    // `{i32Value:x}` on a negative i32 renders its full 64-bit
    // sign-extended pattern (16 hex digits), not the narrower 8 a real
    // 32-bit-only reinterpretation would give. The compiled backend
    // matches this exactly (see registerFormatRuntime's own identical
    // choice), so the two backends still agree byte-for-byte, which is
    // what actually matters.
    std::string formatValue(const Value& value, const FormatSpec& spec)
    {
        char buf[128];
        std::string fmt = "%";
        if (spec.zeroPad)
        {
            fmt += '0';
        }
        if (spec.width > 0)
        {
            fmt += std::to_string(spec.width);
        }

        if (spec.type == 'b')
        {
            // No printf specifier for binary - hand-rolled, mirroring
            // registerFormatRuntime's own identical bit-extraction loop.
            const auto bits = static_cast<std::uint64_t>(std::get<std::int64_t>(value));
            std::string digits;
            if (bits == 0)
            {
                digits = "0";
            }
            else
            {
                std::uint64_t remaining = bits;
                while (remaining > 0)
                {
                    digits = static_cast<char>('0' + (remaining & 1)) + digits;
                    remaining >>= 1;
                }
            }
            if (static_cast<int>(digits.size()) < spec.width)
            {
                const char pad = spec.zeroPad ? '0' : ' ';
                digits = std::string(spec.width - digits.size(), pad) + digits;
            }
            return digits;
        }

        if (spec.type == 'x' || spec.type == 'X' || spec.type == 'o')
        {
            fmt += "ll";
            fmt += spec.type == 'x' ? 'x' : spec.type == 'X' ? 'X' : 'o';
            const auto bits = static_cast<unsigned long long>(std::get<std::int64_t>(value));
            std::snprintf(buf, sizeof(buf), fmt.c_str(), bits);
            return std::string(buf);
        }

        if (spec.precision.has_value())
        {
            fmt += '.';
            fmt += std::to_string(*spec.precision);
            fmt += 'f';
            std::snprintf(buf, sizeof(buf), fmt.c_str(), std::get<double>(value));
            return std::string(buf);
        }

        // Plain width/zero-pad, decimal (TypeChecker already guarantees
        // an i32/i64 value here).
        fmt += "lld";
        std::snprintf(
            buf, sizeof(buf), fmt.c_str(), static_cast<long long>(std::get<std::int64_t>(value)));
        return std::string(buf);
    }

    // `{expr:spec}` with an explicit alignment char (see
    // docs/language/0057-alignment.md) - the piece's own *unpadded* text,
    // width/zero-pad stripped out of `spec` first (padToWidth applies
    // width separately, as a generic post-processing step over the
    // resulting text - see its own comment for why that has to be a
    // second step here, unlike formatValue's single-pass sprintf above).
    // A radix conversion or precision still delegates to formatValue
    // itself (reusing its existing hex/octal/binary/precision text
    // computation exactly, just with width/zeroPad zeroed out first, so
    // there's no second copy of that conversion logic); a bare value with
    // neither (the case formatValue's own "plain" branch can't handle at
    // all, since it assumes a numeric Value) falls back to the same
    // generic toString() every unformatted interpolation piece already
    // uses - alignment, unlike the rest of this file's numeric-only
    // formatting, applies to any text-representable type (see
    // TypeChecker's own identical relaxation).
    std::string formatValueCore(const Value& value, const FormatSpec& spec)
    {
        if (spec.type != '\0' || spec.precision.has_value())
        {
            FormatSpec unpadded = spec;
            unpadded.width = 0;
            unpadded.zeroPad = false;
            return formatValue(value, unpadded);
        }
        return toString(value);
    }

    // Pads `text` to `width` with spaces per `align` ('<' left, '>'
    // right, '^' center) - a no-op if `text` is already at least `width`
    // long (see docs/language/0057-alignment.md; no truncation is ever
    // performed, matching every other width-related format spec in this
    // file). For '^' with an odd amount of padding, the extra space goes
    // on the right - an arbitrary but consistent tie-break, matched
    // exactly by LlvmIrEmitter's own identical choice so the two backends
    // never disagree on it.
    std::string padToWidth(const std::string& text, char align, int width)
    {
        const int deficit = width - static_cast<int>(text.size());
        if (deficit <= 0)
        {
            return text;
        }
        if (align == '<')
        {
            return text + std::string(deficit, ' ');
        }
        if (align == '>')
        {
            return std::string(deficit, ' ') + text;
        }
        const int left = deficit / 2;
        const int right = deficit - left;
        return std::string(left, ' ') + text + std::string(right, ' ');
    }

    // `{expr:?}` (see docs/language/0058-debug-formatting.md) - identical
    // to the unformatted `toString` for every type except str/String,
    // which get wrapped in double quotes (no internal escaping of
    // embedded quotes/backslashes - real further work, not built this
    // phase, since nothing in the source doc's own example asks for it).
    std::string toStringDebug(const Value& value)
    {
        if (const auto* string = std::get_if<std::string>(&value))
        {
            return "\"" + *string + "\"";
        }
        if (const auto* ownedString = std::get_if<std::shared_ptr<StringInstance>>(&value))
        {
            return "\"" + (*ownedString)->data + "\"";
        }
        return toString(value);
    }
} // namespace

namespace
{
    // The most recently run Interpreter still alive, if any (see
    // docs/language/0062-display-trait.md's own Design section) - the
    // free `toString` function below has no Interpreter of its own to
    // call `callFunction` through, so it reaches one via this pointer
    // instead. Set unconditionally at the top of Interpreter::run()
    // (never reset there - `main.cpp`'s own top-level auto-print of a
    // struct binding calls toString *after* run() has already returned,
    // while the Interpreter instance itself is still alive, and needs
    // Display dispatch to work there too, not just for print/
    // interpolation evaluated during run() itself). Reset instead by
    // `~Interpreter()`, and only if it still points at *this* instance -
    // which is what actually makes a dangling read structurally
    // impossible: the one real risk (a test harness pattern like
    // `toString(runProgram(source).at("x"))`, where the Interpreter goes
    // out of scope and is destroyed before toString is ever called on
    // the value it produced) is exactly the case the destructor guards
    // against, on every return path including an exception unwind out of
    // run() itself.
    Interpreter* g_activeInterpreter = nullptr;
} // namespace

std::string toString(const Value& value)
{
    if (const auto* integer = std::get_if<std::int64_t>(&value))
    {
        return std::to_string(*integer);
    }
    if (const auto* floating = std::get_if<double>(&value))
    {
        // "%g" (not "%f") - matches LlvmIrEmitter's own @axea.f64.to_str,
        // which also formats via a real sprintf("%g", ...) call, so
        // interpreted and compiled output stay character-for-character
        // identical (see docs/language/0005-type-system.md).
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", *floating);
        return buf;
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
    if (const auto* optional = std::get_if<std::shared_ptr<OptionalInstance>>(&value))
    {
        // "Some(<payload>)"/"None" (see docs/language/0052-optional.md) -
        // matches LlvmIrEmitter's own @axea.optional.<id>.to_str exactly
        // (same text, verified via the usual diff-against-compiled-output
        // discipline), so top-level printing of a raw Optional<T> binding
        // agrees byte-for-byte between the interpreter and the compiled
        // backend.
        return (*optional)->hasValue ? "Some(" + toString((*optional)->value) + ")" : "None";
    }
    if (const auto* result = std::get_if<std::shared_ptr<ResultInstance>>(&value))
    {
        // "Ok(<payload>)"/"Err(<payload>)" (see docs/language/0063-result.md) -
        // matches LlvmIrEmitter's own @axea.result.<id>.to_str exactly, the
        // same "verified byte-for-byte against the compiled backend" doc
        // convention Optional's own toString case already established.
        return (*result)->isOk ? "Ok(" + toString((*result)->okValue) + ")"
                               : "Err(" + toString((*result)->errValue) + ")";
    }
    if (const auto* enumInstance = std::get_if<std::shared_ptr<EnumInstance>>(&value))
    {
        // "VariantName(field0, field1, ...)", bare "VariantName" for a no-payload variant (see
        // docs/language/0064-enums.md) - matches LlvmIrEmitter's own @axea.tostring.<name>
        // exactly, the same byte-for-byte-verified convention Optional/Result's own toString
        // cases already established. Unlike Result<T,E>'s own printing, no payload-type
        // restriction: every field is stringified recursively via this same toString, so any
        // printable type works as a variant's own payload.
        std::string result = (*enumInstance)->variantName;
        if (!(*enumInstance)->fields.empty())
        {
            result += "(";
            for (std::size_t i = 0; i < (*enumInstance)->fields.size(); ++i)
            {
                if (i > 0)
                {
                    result += ", ";
                }
                result += toString((*enumInstance)->fields[i]);
            }
            result += ")";
        }
        return result;
    }
    if (const auto* instance = std::get_if<std::shared_ptr<StructInstance>>(&value))
    {
        // Display trait dispatch (see docs/language/0062-display-trait.md) -
        // checked first, before the default per-field printer built
        // below; a struct with no registered `impl Display` (the common
        // case) falls straight through unchanged.
        if (g_activeInterpreter)
        {
            if (auto formatted = g_activeInterpreter->tryFormatStructWithDisplay(*instance))
            {
                return *formatted;
            }
        }
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
        // Reachable for real now (see docs/language/0056-slice-printing.md)
        // via print/write/interpolation of a slice<T>-typed parameter,
        // identically to an array - this branch existed since
        // docs/language/0032-slices.md and needed no change at all, only
        // TypeChecker::isTextRepresentable's own gate did.
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

bool ValueLess::operator()(const Value& a, const Value& b) const
{
    // str compares by its own real std::string `<` (lexicographic byte
    // order); f64 by its own real `double` `<` (IEEE 754 ordered compare -
    // see docs/language/0005-type-system.md); i32/i64/char all reduce to
    // `asInt` - the same numeric unification `asInt` already provides for
    // arithmetic/equality elsewhere in this file (see
    // docs/language/0044-char.md). Checking only `a`'s own alternative is
    // safe: TypeChecker::isOrderableKind already guarantees both sides are
    // the same orderable kind before a well-typed program's Value ever
    // reaches here.
    if (const auto* str = std::get_if<std::string>(&a))
    {
        return *str < std::get<std::string>(b);
    }
    if (const auto* f = std::get_if<double>(&a))
    {
        return *f < std::get<double>(b);
    }
    return asInt(a) < asInt(b);
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

Interpreter::~Interpreter()
{
    // See g_activeInterpreter's own comment above toString.
    if (g_activeInterpreter == this)
    {
        g_activeInterpreter = nullptr;
    }
}

void Interpreter::run(const Program& program)
{
    g_activeInterpreter = this;

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
            // is registered exactly like a top-level FunctionDecl;
            // "Display"'s own "format" additionally populates
            // displayImpls_, the one map toString's dispatch above
            // actually consults.
            for (const auto& method : implDecl->methods)
            {
                functions_[method->name] = method.get();
            }
            if (implDecl->traitName == "Display")
            {
                const std::string formatName = implDecl->typeName + ".format";
                if (const auto it = functions_.find(formatName); it != functions_.end())
                {
                    displayImpls_[implDecl->typeName] = it->second;
                }
            }
        }
    }

    for (const auto& item : program.items)
    {
        if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(item.get()))
        {
            execute(*assignment, globalEnv_);
        }
        else if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(item.get()))
        {
            // A bare top-level call kept for its side effect
            // (`print("hi")`, `write("...")`) - see
            // Parser::looksLikeFunctionDecl's own doc comment.
            execute(*exprStmt, globalEnv_);
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
        if (assignment->declaredType)
        {
            value = wrapForUnion(std::move(value), *assignment->declaredType);
        }
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

    // i32 and i64 share the same std::int64_t Value alternative (see
    // Interpreter.hpp) - this interpreter never distinguishes their width
    // at runtime, only TypeChecker does.
    if (const auto* int64Expr = dynamic_cast<const Int64Expr*>(&expr))
    {
        return int64Expr->value;
    }

    if (const auto* floatExpr = dynamic_cast<const FloatExpr*>(&expr))
    {
        return floatExpr->value;
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

    // `"Hello {name}"` (see docs/language/Axea_Printing_Formatting.md) -
    // unlike the LLVM backend (which desugars into real Buffer IR
    // instructions to stay consistent with how the compiled backend
    // would want to implement it efficiently), the interpreter just
    // concatenates directly: each piece's own content, via the exact
    // same `toString` free function every other print path here already
    // shares, into one std::string, wrapped in a fresh StringInstance -
    // matching TypeChecker's own "the result is an owned String" rule.
    if (const auto* interpolated = dynamic_cast<const InterpolatedStringExpr*>(&expr))
    {
        std::string content;
        for (const auto& piece : interpolated->pieces)
        {
            if (!piece.expr)
            {
                content += piece.literalText;
                continue;
            }
            if (!piece.selfDocPrefix.empty())
            {
                content += piece.selfDocPrefix + "=";
            }
            const Value pieceValue = evaluate(*piece.expr, env);
            if (piece.debug)
            {
                content += toStringDebug(pieceValue);
            }
            else if (piece.formatSpec.empty())
            {
                content += toString(pieceValue);
            }
            else
            {
                const FormatSpec spec = parseFormatSpec(piece.formatSpec);
                content +=
                    spec.align == '\0'
                        ? formatValue(pieceValue, spec)
                        : padToWidth(formatValueCore(pieceValue, spec), spec.align, spec.width);
            }
        }
        return std::make_shared<StringInstance>(StringInstance{std::move(content)});
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
        // `print`/`write` (see docs/language/Axea_Printing_Formatting.md)
        // - compiler builtins, checked before the ordinary functions_
        // lookup below (TypeChecker::registerSignatures already
        // guarantees no user declaration can shadow either name). Each
        // argument stringified via the exact same `toString` free
        // function every other print path in this interpreter already
        // uses (top-level bindings, struct/array/List element printing),
        // so this needed no new stringification logic at all - unlike
        // the LLVM backend, which needed dedicated i32/bool-to-string
        // runtime routines since it has no equivalent of a single
        // generic toString to fall back on.
        if (call->callee == "print" || call->callee == "write")
        {
            for (std::size_t i = 0; i < call->arguments.size(); ++i)
            {
                if (i > 0)
                {
                    std::cout << ' ';
                }
                std::cout << toString(evaluate(*call->arguments[i], env));
            }
            if (call->callee == "print")
            {
                std::cout << '\n';
            }
            return Value{std::monostate{}};
        }

        const auto it = functions_.find(call->callee);
        if (it == functions_.end())
        {
            // extern c functions (see docs/language/0048-ffi.md) - the
            // interpreter can't dynamically link against arbitrary C
            // symbols, so only a small, explicit allowlist of well-known
            // libc functions is hand-implemented (callExtern below),
            // matching their real observable behavior exactly - verified
            // directly against the compiled backend, which links against
            // the genuine libc symbol. Any other extern name is a clear,
            // honest runtime error rather than silently doing nothing.
            std::vector<Value> externArgs;
            externArgs.reserve(call->arguments.size());
            for (const auto& argument : call->arguments)
            {
                externArgs.push_back(evaluate(*argument, env));
            }
            return callExtern(call->callee, externArgs);
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
            // Implicit union wrapping (see docs/language/0065-unions.md) -
            // `f(5)`/`f("hi")` against `f(x: i32 | str)` need no wrapper
            // syntax.
            if (i < it->second->params.size())
            {
                argValue = wrapForUnion(std::move(argValue), it->second->params[i].type);
            }
            args.push_back(std::move(argValue));
        }

        return callFunction(*it->second, std::move(args));
    }

    if (const auto* methodCall = dynamic_cast<const MethodCallExpr*>(&expr))
    {
        // `EnumName.Variant(args)` construction (see docs/language/0064-enums.md) - checked
        // before evaluating `methodCall->object` as an ordinary value just below, which would
        // otherwise throw "undefined variable" trying to look up a bare enum type name in the
        // environment. TypeChecker already guarantees this is well-formed (real variant, right
        // arity/types) whenever it's reached, so no further validation happens here.
        if (const auto* name = dynamic_cast<const NameExpr*>(methodCall->object.get()))
        {
            if (const auto enumIt = enums_.find(name->name); enumIt != enums_.end())
            {
                std::vector<Value> fields;
                fields.reserve(methodCall->arguments.size());
                for (const auto& argument : methodCall->arguments)
                {
                    fields.push_back(evaluate(*argument, env));
                }
                return std::make_shared<EnumInstance>(
                    EnumInstance{enumIt->second->name, methodCall->method, std::move(fields)});
            }
        }

        auto objectValue = evaluate(*methodCall->object, env);

        // `.parse<T>()` (see docs/language/0046-generic-methods.md and
        // docs/language/0052-optional.md) - the first generic method call
        // in this codebase, checked before the object-type-keyed dispatch
        // chain below for the same reason TypeChecker checks it first.
        // Returns Optional<T>, not T directly, with real success/failure
        // detection (invalid input is a genuine None now, not a silently-
        // returned fallback) - mirrors LlvmIrEmitter's own
        // @axea.parse.i32/@axea.parse.bool logic exactly (same leading-'-'
        // handling, same digit loop, same full-string-consumption success
        // rule, same "must be exactly 'true'/'false'" bool rule) so the
        // interpreter and the compiled backend agree byte-for-byte,
        // verified directly via the usual diff-against-compiled-output
        // discipline.
        if (methodCall->method == "parse")
        {
            const std::string content = asStrContent(objectValue);
            if (methodCall->typeArgument == "i32" || methodCall->typeArgument == "i64")
            {
                // Both share the interpreter's own std::int64_t Value
                // alternative (see docs/language/0051-numeric-widening.md)
                // - the identical hand-rolled digit loop parses either;
                // only TypeChecker distinguishes their width.
                std::size_t idx = 0;
                bool negative = false;
                if (!content.empty() && content[0] == '-')
                {
                    negative = true;
                    idx = 1;
                }
                std::size_t digitStart = idx;
                std::int64_t acc = 0;
                while (idx < content.size() && content[idx] >= '0' && content[idx] <= '9')
                {
                    acc = acc * 10 + (content[idx] - '0');
                    ++idx;
                }
                const bool ok = idx > digitStart && idx == content.size();
                const Value result = negative ? -acc : acc;
                return std::make_shared<OptionalInstance>(
                    OptionalInstance{ok, ok ? result : Value{std::monostate{}}});
            }
            if (methodCall->typeArgument == "f64")
            {
                // A real strtod call, mirroring LlvmIrEmitter's own
                // @axea.parse.f64 exactly (real libc decimal-to-binary
                // parsing, not a hand-rolled algorithm - see
                // registerParseRuntime's own comment for why) - both
                // backends agree byte-for-byte since both ultimately call
                // the identical underlying libc routine. `endptr` (unused
                // before Optional<T> existed) now drives real success
                // detection: ok iff something was consumed *and* the
                // entire string was consumed, matching @axea.parse.f64's
                // own identical rule.
                char* endptr = nullptr;
                const double result = std::strtod(content.c_str(), &endptr);
                const bool ok =
                    endptr != content.c_str() && endptr == content.c_str() + content.size();
                return std::make_shared<OptionalInstance>(
                    OptionalInstance{ok, ok ? Value{result} : Value{std::monostate{}}});
            }
            if (methodCall->typeArgument == "bool")
            {
                if (content == "true")
                {
                    return std::make_shared<OptionalInstance>(OptionalInstance{true, Value{true}});
                }
                if (content == "false")
                {
                    return std::make_shared<OptionalInstance>(OptionalInstance{true, Value{false}});
                }
                return std::make_shared<OptionalInstance>(
                    OptionalInstance{false, Value{std::monostate{}}});
            }
            throw std::runtime_error("parse<" + methodCall->typeArgument + "> is not supported");
        }

        // `.to_cstr()` (see docs/language/0048-ffi.md) - a pure identity:
        // cstr and str share the exact same interpreter representation
        // (a bare std::string), so this is just asStrContent's own
        // existing str-coercion, with no further transformation needed.
        if (methodCall->method == "to_cstr")
        {
            return asStrContent(objectValue);
        }

        // `.unwrap_or(default)`/`.is_some()`/`.is_none()` (see
        // docs/language/0052-optional.md) - the non-propagating half of
        // Optional<T>'s API, checked here for the same reason "parse"/
        // "to_cstr" above are (TypeChecker checks them before the
        // per-kind dispatch chain too). `unwrap_or` is shared with
        // Result<T,E> (see docs/language/0063-result.md) - TypeChecker
        // already guarantees `objectValue` is one or the other, so a
        // `get_if` check on the runtime shape (rather than a static
        // "which kind" flag threaded through, which the interpreter has
        // no equivalent of) is all that's needed to dispatch correctly.
        if (methodCall->method == "unwrap_or")
        {
            if (const auto* optional = std::get_if<std::shared_ptr<OptionalInstance>>(&objectValue))
            {
                return (*optional)->hasValue ? (*optional)->value
                                             : evaluate(*methodCall->arguments.front(), env);
            }
            const auto& result = std::get<std::shared_ptr<ResultInstance>>(objectValue);
            return result->isOk ? result->okValue : evaluate(*methodCall->arguments.front(), env);
        }
        if (methodCall->method == "is_some" || methodCall->method == "is_none")
        {
            const auto& optional = std::get<std::shared_ptr<OptionalInstance>>(objectValue);
            return methodCall->method == "is_some" ? optional->hasValue : !optional->hasValue;
        }
        if (methodCall->method == "is_ok" || methodCall->method == "is_err")
        {
            const auto& result = std::get<std::shared_ptr<ResultInstance>>(objectValue);
            return methodCall->method == "is_ok" ? result->isOk : !result->isOk;
        }

        // `.join(separator)` (see
        // docs/language/0050-collection-join-and-slicing.md) - applies
        // across both Array and List (asIndexable, checked before the
        // per-kind dispatch chain below, same placement reasoning as
        // "parse"/"to_cstr" above). Reuses the interpreter's own generic
        // toString(), exactly like print()/interpolation already do -
        // TypeChecker::isTextRepresentable already guarantees every
        // element here is one toString() already handles.
        if (methodCall->method == "join")
        {
            auto indexable = asIndexable(objectValue);
            const std::string separator =
                asStrContent(evaluate(*methodCall->arguments.front(), env));
            std::string result;
            for (std::size_t i = 0; i < indexable->length; ++i)
            {
                if (i > 0)
                {
                    result += separator;
                }
                result += toString((*indexable->elements)[i]);
            }
            return std::make_shared<StringInstance>(StringInstance{std::move(result)});
        }

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
                Value key = evaluate(*methodCall->arguments[0], env);
                Value value = evaluate(*methodCall->arguments[1], env);
                (*sortedMap)->entries[std::move(key)] = std::move(value);
                return Value{std::monostate{}};
            }
            if (methodCall->method == "get")
            {
                const Value key = evaluate(*methodCall->arguments.front(), env);
                const auto it = (*sortedMap)->entries.find(key);
                if (it == (*sortedMap)->entries.end())
                {
                    throw std::runtime_error("SortedMap.get on a missing key");
                }
                return it->second;
            }
            if (methodCall->method == "contains")
            {
                const Value key = evaluate(*methodCall->arguments.front(), env);
                return (*sortedMap)->entries.contains(key);
            }
            if (methodCall->method == "remove")
            {
                const Value key = evaluate(*methodCall->arguments.front(), env);
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
                Value value = evaluate(*methodCall->arguments.front(), env);
                (*sortedSet)->elements.insert(std::move(value));
                return Value{std::monostate{}};
            }
            if (methodCall->method == "contains")
            {
                const Value value = evaluate(*methodCall->arguments.front(), env);
                return (*sortedSet)->elements.contains(value);
            }
            if (methodCall->method == "remove")
            {
                const Value value = evaluate(*methodCall->arguments.front(), env);
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
        // amortized-growth text-construction type. `data` itself just
        // rides std::string's own `+=`/`clear` machinery (content is
        // content, regardless of which growth algorithm produced the
        // storage behind it) - but `capacity` is tracked explicitly,
        // via growBufferCapacity, mirroring ensureBufferCapacity's own
        // doubling rule bit for bit rather than reading
        // `data.capacity()` (previously the case, and wrong - see
        // BufferInstance::capacity's own comment for why).
        if (const auto* buffer = std::get_if<std::shared_ptr<BufferInstance>>(&objectValue))
        {
            // "write" (see docs/language/0061-buffer-write.md) is a plain
            // alias of "append" - string interpolation already lowers at
            // parse time for any string literal argument, regardless of
            // which method receives it, so there is no runtime behavior
            // for "write" to add beyond the naming convention.
            if (methodCall->method == "append" || methodCall->method == "write")
            {
                const std::string text =
                    asStrContent(evaluate(*methodCall->arguments.front(), env));
                growBufferCapacity(
                    **buffer, static_cast<std::int64_t>((*buffer)->data.size() + text.size() + 1));
                (*buffer)->data += text;
                return Value{std::monostate{}};
            }
            if (methodCall->method == "append_line")
            {
                const std::string text =
                    asStrContent(evaluate(*methodCall->arguments.front(), env));
                // +1 for the appended '\n', +1 for the null terminator -
                // matches emitBufferAppendLine's own single combined
                // growth check exactly (not two separate ones).
                growBufferCapacity(
                    **buffer, static_cast<std::int64_t>((*buffer)->data.size() + text.size() + 2));
                (*buffer)->data += text;
                (*buffer)->data += '\n';
                return Value{std::monostate{}};
            }
            if (methodCall->method == "clear")
            {
                // Length only - capacity is deliberately untouched,
                // mirroring emitBufferClear's own identical choice (the
                // underlying storage isn't shrunk/reallocated, just
                // logically emptied).
                (*buffer)->data.clear();
                return Value{std::monostate{}};
            }
            if (methodCall->method == "reserve")
            {
                // No +1 here - emitBufferReserve passes its own argument
                // straight through to ensureBufferCapacity unmodified,
                // unlike append/append_line's own null-terminator-
                // inclusive `needed`.
                const std::int64_t capacity = asInt(evaluate(*methodCall->arguments.front(), env));
                growBufferCapacity(**buffer, capacity);
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
                // in a merely "unspecified" state). Capacity resets to 1
                // too, matching emitBufferFinish's own identical reset.
                auto result =
                    std::make_shared<StringInstance>(StringInstance{std::move((*buffer)->data)});
                (*buffer)->data = std::string();
                (*buffer)->capacity = 1;
                return Value{result};
            }
            throw std::runtime_error("no such method: " + methodCall->method);
        }

        throw std::runtime_error("no such method '" + methodCall->method + "' on this value");
    }

    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        // `EnumName.Variant` (no parens - a no-payload variant, see
        // docs/language/0064-enums.md and MethodCallExpr's own identical check just above).
        if (const auto* name = dynamic_cast<const NameExpr*>(field->object.get()))
        {
            if (const auto enumIt = enums_.find(name->name); enumIt != enums_.end())
            {
                return std::make_shared<EnumInstance>(
                    EnumInstance{enumIt->second->name, field->field, {}});
            }
        }

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
        // A bare str's `.length`/`.bytes` (see docs/language/0047-unicode.md)
        // - previously unsupported entirely (str had no field access at
        // all); now added alongside the same swap String/Buffer get below,
        // since all three text types share the same "count codepoints, not
        // bytes, by default" story.
        if (const auto* str = std::get_if<std::string>(&objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>(countCodepoints(*str));
            }
            if (field->field == "bytes")
            {
                return static_cast<std::int64_t>(str->size());
            }
            throw std::runtime_error("no such field: " + field->field);
        }

        // `.length` now counts Unicode codepoints, not bytes - `.bytes` is
        // the new name for what `.length` used to return (see
        // docs/language/0047-unicode.md). A real, deliberate behavior
        // change to already-shipped semantics (docs/language/0042-string.md's
        // own "byte count, not codepoint count" framing is now reversed).
        if (const auto* string = std::get_if<std::shared_ptr<StringInstance>>(&objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>(countCodepoints((*string)->data));
            }
            if (field->field == "bytes")
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
        // above); `.length`/`.bytes` are not expected to diverge and must
        // match exactly. Same `.length`-now-counts-codepoints swap as
        // String above.
        if (const auto* buffer = std::get_if<std::shared_ptr<BufferInstance>>(&objectValue))
        {
            if (field->field == "length")
            {
                return static_cast<std::int64_t>(countCodepoints((*buffer)->data));
            }
            if (field->field == "bytes")
            {
                return static_cast<std::int64_t>((*buffer)->data.size());
            }
            if (field->field == "capacity")
            {
                // (*buffer)->capacity, not (*buffer)->data.capacity() -
                // see BufferInstance::capacity's own comment for why
                // those are genuinely different quantities.
                return (*buffer)->capacity;
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

        // Single-character indexing (`s[i]`) on a str-coercible value - a
        // real Unicode codepoint index (codepointAt), not a byte offset
        // (see docs/language/0047-unicode.md). Checked before
        // asIndexable, since str/String aren't Indexable in that sense (a
        // raw std::string/shared_ptr<StringInstance>, not a
        // std::vector<Value>).
        if (isStrCoercibleValue(objectValue))
        {
            const std::int64_t indexValue = asInt(evaluate(*index->index, env));
            return codepointAt(asStrContent(objectValue), indexValue);
        }

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

    if (const auto* strSlice = dynamic_cast<const StrSliceExpr*>(&expr))
    {
        // A real substring/sublist copy, not a zero-copy view - see
        // docs/language/0045-str-slicing.md. Unlike the LLVM backend
        // (which never bounds-checks - matches every other out-of-bounds
        // case there), this validates the range at runtime, the same
        // "interpreter checks, compiled code does not" split every other
        // indexing operation here already follows. Widened in
        // docs/language/0050-collection-join-and-slicing.md: an Array/List
        // `object` (checked first via asIndexable, since asStrContent
        // would otherwise throw on a non-str value) slices into a fresh
        // List instead of a fresh str.
        Value objectValue = evaluate(*strSlice->object, env);
        if (auto indexable = asIndexable(objectValue))
        {
            const auto length = static_cast<std::int64_t>(indexable->length);
            const std::int64_t start = strSlice->start ? asInt(evaluate(*strSlice->start, env)) : 0;
            const std::int64_t end = strSlice->end ? asInt(evaluate(*strSlice->end, env)) : length;
            if (start < 0 || end > length || start > end)
            {
                throw std::runtime_error("invalid slice range [" + std::to_string(start) + ".." +
                                         std::to_string(end) + "] for a collection of length " +
                                         std::to_string(length));
            }
            auto sliced = std::make_shared<ListInstance>();
            sliced->elements.assign(indexable->elements->begin() + static_cast<std::size_t>(start),
                                    indexable->elements->begin() + static_cast<std::size_t>(end));
            return sliced;
        }

        const std::string content = asStrContent(objectValue);
        const auto length = static_cast<std::int64_t>(content.size());
        const std::int64_t start = strSlice->start ? asInt(evaluate(*strSlice->start, env)) : 0;
        const std::int64_t end = strSlice->end ? asInt(evaluate(*strSlice->end, env)) : length;
        if (start < 0 || end > length || start > end)
        {
            throw std::runtime_error("invalid slice range [" + std::to_string(start) + ".." +
                                     std::to_string(end) + "] for str of length " +
                                     std::to_string(length));
        }
        return content.substr(static_cast<std::size_t>(start),
                              static_cast<std::size_t>(end - start));
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
            // f64 (a `double` Value) gets real floating-point arithmetic;
            // i32/i64 (both the std::int64_t alternative - see
            // Interpreter.hpp) share the existing asInt-based integer path,
            // exactly as before this phase. TypeChecker::requireInt already
            // guarantees both operands are the same numeric kind, so
            // checking only `left` is enough (mirrors ValueLess's own
            // "check one side" reasoning below).
            case TokenKind::Plus:
                if (const auto* lf = std::get_if<double>(&left))
                {
                    return *lf + std::get<double>(right);
                }
                return asInt(left) + asInt(right);
            case TokenKind::Minus:
                if (const auto* lf = std::get_if<double>(&left))
                {
                    return *lf - std::get<double>(right);
                }
                return asInt(left) - asInt(right);
            case TokenKind::Star:
                if (const auto* lf = std::get_if<double>(&left))
                {
                    return *lf * std::get<double>(right);
                }
                return asInt(left) * asInt(right);
            case TokenKind::Slash:
                // Real IEEE 754 division for f64 - unlike integer division,
                // a zero divisor is not an error (yields +-inf/NaN), so no
                // zero-check here, unlike the integer path just below.
                if (const auto* lf = std::get_if<double>(&left))
                {
                    return *lf / std::get<double>(right);
                }
                if (asInt(right) == 0)
                {
                    throw std::runtime_error("division by zero");
                }
                return asInt(left) / asInt(right);
            case TokenKind::EqualEqual:
                if (isStrCoercibleValue(left))
                {
                    return asStrContent(left) == asStrContent(right);
                }
                return left == right;
            case TokenKind::BangEqual:
                if (isStrCoercibleValue(left))
                {
                    return asStrContent(left) != asStrContent(right);
                }
                return !(left == right);
            // Derived from a single ValueLess "less-than" the same way
            // LessEqual/Greater/GreaterEqual are standard textbook
            // derivatives of `<` for any total order.
            case TokenKind::Less: return ValueLess{}(left, right);
            case TokenKind::LessEqual: return !ValueLess{}(right, left);
            case TokenKind::Greater: return ValueLess{}(right, left);
            case TokenKind::GreaterEqual: return !ValueLess{}(left, right);
            default: throw std::runtime_error("unsupported operator");
        }
    }

    // `<expr> as <targetType>` (see docs/language/0005-type-system.md) -
    // TypeChecker::checkExpr's own CastExpr case already guarantees both
    // the operand's type and targetType are i32/i64/f64, so only three
    // shapes reach here: int-to-int (i32<->i64, both the same
    // std::int64_t Value alternative - a real no-op at this level, unlike
    // the LLVM backend's own sext/trunc), int-to-float, and
    // float-to-int (truncating toward zero, matching C++'s own
    // double-to-integral conversion semantics).
    if (const auto* cast = dynamic_cast<const CastExpr*>(&expr))
    {
        const Value operand = evaluate(*cast->operand, env);
        if (cast->targetType == "f64")
        {
            if (const auto* f = std::get_if<double>(&operand))
            {
                return *f;
            }
            return static_cast<double>(asInt(operand));
        }
        // i32 or i64 target.
        if (const auto* f = std::get_if<double>(&operand))
        {
            return static_cast<std::int64_t>(*f);
        }
        return asInt(operand);
    }

    if (const auto* someExpr = dynamic_cast<const SomeExpr*>(&expr))
    {
        return std::make_shared<OptionalInstance>(
            OptionalInstance{true, evaluate(*someExpr->value, env)});
    }

    if (dynamic_cast<const NoneExpr*>(&expr))
    {
        // Unlike the LLVM backend, the interpreter's Value is dynamically
        // typed - None needs no payload-type context to construct (see
        // docs/language/0052-optional.md and IrGenerator's own
        // optionalPayloadTypeName helper, which exists purely because the
        // LLVM backend's {i1, T} struct literal needs a concrete T even
        // when hasValue is false).
        return std::make_shared<OptionalInstance>(OptionalInstance{false, Value{std::monostate{}}});
    }

    if (const auto* okExpr = dynamic_cast<const OkExpr*>(&expr))
    {
        // See docs/language/0063-result.md. errValue stays a default-
        // constructed Value (never read - every consumer checks isOk
        // first, mirroring OptionalInstance's own "never read in that
        // state" convention).
        return std::make_shared<ResultInstance>(
            ResultInstance{true, evaluate(*okExpr->value, env), Value{std::monostate{}}});
    }

    if (const auto* errExpr = dynamic_cast<const ErrExpr*>(&expr))
    {
        return std::make_shared<ResultInstance>(
            ResultInstance{false, Value{std::monostate{}}, evaluate(*errExpr->value, env)});
    }

    if (const auto* tryExpr = dynamic_cast<const TryExpr*>(&expr))
    {
        // Generalized to Result<T,E> (see docs/language/0063-result.md) -
        // the interpreter's Value is dynamically typed, so (unlike
        // IrGenerator, which has to decide Optional-vs-Result from the
        // enclosing function's own declared return type string) this just
        // inspects which shape `operandValue` actually is at runtime.
        const Value operandValue = evaluate(*tryExpr->operand, env);
        if (const auto* optional = std::get_if<std::shared_ptr<OptionalInstance>>(&operandValue))
        {
            if ((*optional)->hasValue)
            {
                return (*optional)->value;
            }
            // Propagates None out of the enclosing function - the
            // language's only expression-context early return (see
            // docs/language/0052-optional.md). Reuses the exact same
            // ReturnSignal `return` itself throws; TypeChecker already
            // guarantees `?` only appears inside a function whose own
            // return type is Optional<U>, so this is always caught at
            // that function's own call boundary, never escaping to
            // top-level code.
            throw ReturnSignal{Value{std::make_shared<OptionalInstance>(
                OptionalInstance{false, Value{std::monostate{}}})}};
        }
        const auto& result = std::get<std::shared_ptr<ResultInstance>>(operandValue);
        if (result->isOk)
        {
            return result->okValue;
        }
        // Propagates the *same* Err(e) out of the enclosing function,
        // preserving `e` itself (unlike None, which carries no payload to
        // preserve) - TypeChecker already guarantees the enclosing
        // function's own Err type matches the operand's, so no conversion
        // is needed here either.
        throw ReturnSignal{Value{std::make_shared<ResultInstance>(
            ResultInstance{false, Value{std::monostate{}}, result->errValue})}};
    }

    if (const auto* matchExpr = dynamic_cast<const MatchExpr*>(&expr))
    {
        // See docs/language/0064-enums.md. TypeChecker already guarantees exhaustiveness, a
        // real variant per named arm, and the right binding count, so this just scans arms in
        // written order for the first match (a wildcard "_" always matches) - no further
        // validation needed.
        const Value scrutineeValue = evaluate(*matchExpr->scrutinee, env);
        const auto& scrutinee = std::get<std::shared_ptr<EnumInstance>>(scrutineeValue);
        for (const auto& arm : matchExpr->arms)
        {
            if (arm.variantName != "_" && arm.variantName != scrutinee->variantName)
            {
                continue;
            }
            Environment armEnv(&env);
            for (std::size_t i = 0; i < arm.bindingNames.size(); ++i)
            {
                armEnv.define(arm.bindingNames[i], scrutinee->fields[i]);
            }
            return evaluate(*arm.body, armEnv);
        }
        throw std::runtime_error("no match arm for variant '" + scrutinee->variantName + "'");
    }

    throw std::runtime_error("unsupported expression");
}

Value Interpreter::wrapForUnion(Value value, const std::string& declaredTypeName) const
{
    if (declaredTypeName.find('|') == std::string::npos)
    {
        return value;
    }
    if (const auto* alreadyUnion = std::get_if<std::shared_ptr<EnumInstance>>(&value);
        alreadyUnion && (*alreadyUnion)->typeName == declaredTypeName)
    {
        return value;
    }

    const auto matchesAlternative = [&value](const std::string& alternative)
    {
        if (alternative == "i32" || alternative == "i64")
        {
            return std::holds_alternative<std::int64_t>(value);
        }
        if (alternative == "f64")
        {
            return std::holds_alternative<double>(value);
        }
        if (alternative == "bool")
        {
            return std::holds_alternative<bool>(value);
        }
        if (alternative == "char")
        {
            return std::holds_alternative<char32_t>(value);
        }
        if (alternative == "str" || alternative == "cstr")
        {
            return std::holds_alternative<std::string>(value);
        }
        if (alternative == "String")
        {
            return std::holds_alternative<std::shared_ptr<StringInstance>>(value);
        }
        if (const auto* structValue = std::get_if<std::shared_ptr<StructInstance>>(&value))
        {
            return (*structValue)->typeName == alternative;
        }
        if (const auto* enumValue = std::get_if<std::shared_ptr<EnumInstance>>(&value))
        {
            return (*enumValue)->typeName == alternative;
        }
        return false;
    };

    std::size_t start = 0;
    while (true)
    {
        const auto bar = declaredTypeName.find('|', start);
        const std::string alternative = declaredTypeName.substr(
            start, bar == std::string::npos ? std::string::npos : bar - start);
        if (matchesAlternative(alternative))
        {
            return Value{std::make_shared<EnumInstance>(
                EnumInstance{declaredTypeName, alternative, {std::move(value)}})};
        }
        if (bar == std::string::npos)
        {
            break;
        }
        start = bar + 1;
    }
    // Unreachable once TypeChecker has validated the program (its own
    // isUnionMember already guarantees the value's static type is one of
    // this union's alternatives) - a defensive error, not a real user-facing
    // one, for the same reason every other interpreter-trusts-TypeChecker
    // call site in this file has none either.
    throw std::runtime_error("internal error: value doesn't match any alternative of union " +
                             declaredTypeName);
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
        // Implicit union wrapping (see docs/language/0065-unions.md) - `return 5` from a
        // function declared `-> i32 | str` needs no wrapper syntax.
        if (decl.returnType)
        {
            return wrapForUnion(std::move(signal.value), *decl.returnType);
        }
        return std::move(signal.value);
    }
    return Value{std::monostate{}};
}

std::optional<std::string>
Interpreter::tryFormatStructWithDisplay(const std::shared_ptr<StructInstance>& instance)
{
    const auto it = displayImpls_.find(instance->typeName);
    if (it == displayImpls_.end())
    {
        return std::nullopt;
    }
    // A fresh Buffer, exactly like a real `Buffer()` construction (see
    // BufferNewExpr's own evaluation just above in this file) - `format`
    // writes into it via ordinary `buf.write`/`.append` calls, then this
    // reads the result straight back out, the same "call a function,
    // inspect what it left behind" shape a real Axea caller of
    // `format(point, buf)` would use manually (see this doc's own
    // Design section for why this reuses callFunction wholesale rather
    // than any bespoke dispatch mechanism).
    auto buffer = std::make_shared<BufferInstance>();
    std::vector<Value> args;
    args.push_back(Value{instance});
    args.push_back(Value{buffer});
    callFunction(*it->second, std::move(args));
    return buffer->data;
}

Value Interpreter::callExtern(const std::string& name, const std::vector<Value>& args)
{
    // "puts" (see docs/language/0048-ffi.md) - real libc puts() writes its
    // argument followed by a trailing newline and returns a non-negative
    // int on success; the Axea-level declaration in every example this
    // phase omits a return type (unit/void), so this mirrors only the
    // *observable* behavior (the write), matching what the compiled
    // backend's own `declare void @puts(i8*)` + `call` produces (the real
    // symbol's own int return value is simply never read at that call
    // site either - confirmed directly: declaring an extern void-returning
    // wrapper around a real int-returning libc function links and runs
    // correctly, the caller's own choice not to read a return register is
    // always safe).
    if (name == "puts")
    {
        if (args.size() != 1)
        {
            throw std::runtime_error("extern 'puts' expects 1 argument");
        }
        std::cout << asStrContent(args.front()) << "\n";
        return Value{std::monostate{}};
    }
    // "abs" - real libc abs() returns the absolute value of a real i32.
    // A second allowlist entry (alongside "puts") specifically to
    // demonstrate that more than one extern can be hand-implemented, and
    // that both backends agree on it exactly (see examples/ffi.ax).
    if (name == "abs")
    {
        if (args.size() != 1)
        {
            throw std::runtime_error("extern 'abs' expects 1 argument");
        }
        const std::int64_t value = asInt(args.front());
        return value < 0 ? -value : value;
    }
    throw std::runtime_error("extern function '" + name +
                             "' has no interpreter implementation - only a small allowlist of "
                             "well-known libc functions is hand-implemented this phase");
}

const std::unordered_map<std::string, Value>& Interpreter::variables() const
{
    return globalEnv_.bindings();
}
