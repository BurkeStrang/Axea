#pragma once

#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>

// Numeric interpolation format specs (see
// docs/language/0055-numeric-format-specs.md and
// docs/language/Axea_Printing_Formatting.md's own "Numeric Formatting"
// section), plus alignment (see docs/language/0057-alignment.md and that
// doc's own "Alignment" section) - `{expr:spec}`'s own `spec` text, parsed
// once into this
// structured form and shared by TypeChecker (validation), Interpreter,
// and LlvmIrEmitter (both do real value formatting). Deliberately *not*
// duplicated per this codebase's usual "separate over shared" convention
// for interpreter-vs-compiled-backend logic (kept independent elsewhere
// so a divergence between them is directly, visibly observable): parsing
// "08b" into {zeroPad: true, width: 8, type: 'b'} is pure syntax with no
// runtime behavior to independently verify, so three copies of the
// identical tiny grammar would only risk silently drifting apart, not
// catch a real bug the way duplicated *value*-computation logic does.
struct FormatSpec
{
    bool zeroPad = false;
    int width = 0;
    std::optional<int> precision;
    char type = '\0';  // '\0' = decimal (no radix conversion); else 'x'/'X'/'b'/'o'
    char align = '\0'; // '\0' = none (see docs/language/0057-alignment.md); else '<'/'>'/'^'
};

// Grammar: [align: '<'/'>'/'^'] ['0'] [width digits] ['.' precision digits]
// [type char], type char one of 'x'/'X'/'b'/'o'. Throws a clear error for
// anything else (a malformed spec, an empty precision after '.', a type
// char outside {x,X,b,o}, alignment with no width to align within, '0'
// zero-pad combined with an explicit alignment char - the two are
// mutually exclusive fill strategies, and combining them has no
// well-defined meaning this phase - or trailing garbage) - `text` is
// never itself empty here (the parser already rejects `{expr:}` before
// this is ever called).
inline FormatSpec parseFormatSpec(const std::string& text)
{
    FormatSpec spec;
    std::size_t pos = 0;

    if (pos < text.size() && (text[pos] == '<' || text[pos] == '>' || text[pos] == '^'))
    {
        spec.align = text[pos];
        ++pos;
    }
    if (pos < text.size() && text[pos] == '0')
    {
        spec.zeroPad = true;
        ++pos;
    }
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
    {
        spec.width = spec.width * 10 + (text[pos] - '0');
        ++pos;
    }
    if (spec.align != '\0')
    {
        if (spec.zeroPad)
        {
            throw std::runtime_error("format spec '" + text +
                                     "' cannot combine zero-padding ('0') with an alignment "
                                     "char ('<'/'>'/'^') - the two are mutually exclusive fill "
                                     "strategies");
        }
        if (spec.width == 0)
        {
            throw std::runtime_error("format spec '" + text +
                                     "' has an alignment char with no width to align within");
        }
    }
    if (pos < text.size() && text[pos] == '.')
    {
        ++pos;
        const std::size_t precisionStart = pos;
        int precision = 0;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
        {
            precision = precision * 10 + (text[pos] - '0');
            ++pos;
        }
        if (pos == precisionStart)
        {
            throw std::runtime_error("expected precision digits after '.' in format spec '" + text +
                                     "'");
        }
        spec.precision = precision;
    }
    if (pos < text.size())
    {
        const char c = text[pos];
        if (c != 'x' && c != 'X' && c != 'b' && c != 'o')
        {
            throw std::runtime_error("invalid format spec type '" + std::string(1, c) + "' in '" +
                                     text + "' (expected one of x, X, b, o)");
        }
        spec.type = c;
        ++pos;
    }
    if (pos != text.size())
    {
        throw std::runtime_error("trailing characters in format spec '" + text + "'");
    }
    return spec;
}
