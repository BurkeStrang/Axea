#pragma once

#include "ast/Stmt.hpp"

#include <string>

// `module math` / `use math [as m]` (see docs/language/0066-modules.md) - the multi-file
// discovery + merge step that turns an entry file plus every module it transitively `use`s into
// one flat, already-qualified Program, exactly the shape every later pass (TypeChecker onward)
// already expects and needs zero further module-awareness for beyond recognizing a '.'-qualified
// name (see TypeChecker::registerSignatures's own moduleNames_ derivation). A real, standalone
// compiler component - not folded into main.cpp - both so it's independently testable (see
// tests/ModuleLoaderTests.cpp) and to match this whole codebase's own convention of one class/
// function per pipeline stage.
//
// `rootPath` is always the entry program - its own top-level executable code is what actually
// runs. Every `use`d module is discovered by searching *the entry file's own directory* for a
// `.ax` file whose own `module <name>` declaration matches - module identity is the declared
// name, never the file path (the path is discovery-only, a deliberate design choice: it keeps a
// module's own identity independent of where it happens to live on disk). Transitive: a loaded
// module's own `use` statements are followed the same way, breadth-first. Throws if a named
// module can't be found, or if a module file contains top-level executable code (only the entry
// file's own top-level code is meant to run - see docs/language/0066-modules.md's own Known
// Imprecision for what's deliberately out of scope this phase).
Program loadProgram(const std::string& rootPathText);
