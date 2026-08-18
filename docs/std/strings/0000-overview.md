# Overview

Core types:
- str (partially implemented - see `0001-str.md`)
- String (implemented - see `0002-string.md` and `docs/language/0042-string.md`)
- char (implemented - see `0003-char.md` and `docs/language/0044-char.md`)
- Buffer (implemented - see `0004-buffer.md` and `docs/language/0043-buffer.md`)
- cstr (not implemented - see `0007-ffi.md`)
- slice<u8> (not implemented - see `0006-unicode.md`)

Goals: UTF-8 by default, zero-copy slicing, explicit ownership, efficient APIs.
