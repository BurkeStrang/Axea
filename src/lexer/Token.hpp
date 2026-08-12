#pragma once

#include "TokenKind.hpp"

#include <cstddef>
#include <string>

struct Token
{
    TokenKind kind {TokenKind::Invalid};
    std::string text;
    std::size_t line {1};
    std::size_t column {1};
};
