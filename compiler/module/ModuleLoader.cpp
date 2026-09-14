#include "module/ModuleLoader.hpp"

#include "generics/GenericMonomorphizer.hpp"
#include "lexer/Lexer.hpp"
#include "module/UnqualifiedCallResolver.hpp"
#include "parser/Parser.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace
{
    std::string readFile(const std::string& path)
    {
        std::ifstream input(path);
        if (!input)
        {
            throw std::runtime_error("could not open file: " + path);
        }

        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    Program parseFile(const std::filesystem::path& path)
    {
        // `src` must be a named local, kept alive through lexer.lex()'s own execution - Lexer
        // stores its `source` constructor argument as a non-owning std::string_view. Constructing
        // the Lexer directly from a temporary (`Lexer lexer(readFile(path.string()))`) would leave
        // that view dangling the instant the temporary std::string is destroyed, at the end of
        // that one statement - a real, non-deterministic (heap-layout-dependent) bug this phase's
        // own module-loading pass hit and fixed during its first end-to-end test (see
        // docs/language/0066-modules.md).
        const std::string src = readFile(path.string());
        Lexer lexer(src);
        Parser parser(lexer.lex());
        return parser.parseProgram();
    }

    // Modules (see docs/language/0066-modules.md) - folds `source`'s own items (all declared by
    // `moduleName`, "" for the root/entry file) into `merged`. A FunctionDecl's own `name` is
    // qualified here ("math.sqrt" - the same '.'-mangling ImplDecl methods already use), since it
    // has no external-linkage constraint; an ExternDecl's own `name` is left untouched (it's a
    // real, externally-linked C symbol) and instead gets its `moduleName` field set, so a
    // qualified call site can still recognize it (see ExternDecl::moduleName's own comment).
    // Struct/enum/trait/impl declarations pass through unqualified - cross-module *type* sharing
    // isn't supported this phase (see docs/language/0066-modules.md's own Known Imprecision), so
    // a module's own struct/enum names must not collide with anything else in the final merged
    // program. ModuleDecl/UseDecl are dropped (already fully consumed by this point). A module
    // file's own top-level executable code (AssignmentStmt/ExprStmt) is rejected outright - only
    // the entry file's own top-level code is meant to run.
    void mergeModule(Program& merged, Program source, const std::string& moduleName)
    {
        // A real, previously-undiscovered bug found while porting SortedMap<K,V> (see
        // docs/language/0040-sorted-maps.md's own "2026 Update"): a module's own *private*
        // (non-`pub`) top-level generic function calling ANOTHER private top-level function in
        // that same module, by bare (unqualified) name - e.g. an `impl<K,V> SortedMap<K,V>`
        // method calling its own module's `sortedMapInsertNode<K,V>(...)` helper - never
        // resolved. The loop just below renames every plain top-level FunctionDecl's own
        // *declaration* to "moduleName.originalName", but nothing ever rewrote the matching bare
        // *call sites* to match - UnqualifiedCallResolver::resolveUnqualifiedCalls (which runs
        // later, on the fully merged program) only ever indexes a used module's own `pub`
        // functions as resolution candidates, by design (it exists to expose a module's own
        // public API unqualified to *external* callers, not to fix up a module's own internal
        // cross-references). So GenericMonomorphizer's later functionTemplatesByName lookup
        // (keyed on the now-qualified name) never found the still-bare callee, throwing
        // "'sortedMapInsertNode' is not a known generic function" - a real bug, not specific to
        // SortedMap<K,V> itself (nothing in this file's own Map<K,V>/Set<T> ever had one
        // top-level function call a *different* top-level function - only `self.resize()`-style
        // method calls and `hash<K>()`/`keyEq<K>()` builtin intrinsics - so this gap was never
        // triggered before). Fixed here, narrowly scoped to `source`'s own not-yet-merged items
        // (this exact module's own plain top-level function names only, collected before any of
        // them are renamed below) - zero cross-module ambiguity risk, unlike broadening
        // resolveUnqualifiedCalls' own pub-only index would have been.
        std::unordered_set<std::string> ownFunctionNames;
        for (const auto& item : source.items)
        {
            if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get()))
            {
                ownFunctionNames.insert(function->name);
            }
        }
        if (!moduleName.empty() && !ownFunctionNames.empty())
        {
            for (CallExpr* call : collectAllCallExprs(source.items))
            {
                if (ownFunctionNames.contains(call->callee))
                {
                    call->callee = moduleName + "." + call->callee;
                }
            }
        }

        for (auto& item : source.items)
        {
            if (auto* function = dynamic_cast<FunctionDecl*>(item.get()))
            {
                if (!moduleName.empty())
                {
                    function->name = moduleName + "." + function->name;
                }
                merged.items.push_back(std::move(item));
            }
            else if (auto* externDecl = dynamic_cast<ExternDecl*>(item.get()))
            {
                externDecl->moduleName = moduleName;
                merged.items.push_back(std::move(item));
            }
            else if (dynamic_cast<const StructDecl*>(item.get()) ||
                     dynamic_cast<const EnumDecl*>(item.get()) ||
                     dynamic_cast<const TraitDecl*>(item.get()) ||
                     dynamic_cast<const ImplDecl*>(item.get()))
            {
                merged.items.push_back(std::move(item));
            }
            else if (dynamic_cast<const ModuleDecl*>(item.get()) ||
                     dynamic_cast<const UseDecl*>(item.get()))
            {
                // Already fully consumed - ModuleDecl's own name by this file's own
                // Program::moduleName, UseDecl's own dependency by the caller's worklist scan.
            }
            else if (!moduleName.empty())
            {
                throw std::runtime_error("module '" + moduleName +
                                         "' has top-level code; modules may only contain "
                                         "declarations (struct/enum/function/extern/trait/impl)");
            }
            else
            {
                merged.items.push_back(std::move(item));
            }
        }
    }
} // namespace

namespace
{
    // Modules (see docs/language/0066-modules.md) - the entry file's own directory is always
    // searched first (a module can always be a plain sibling file), but a real project puts
    // shared library code somewhere *other* than wherever a particular program happens to live
    // (e.g. this repo's own `std/`, used by examples/modules/main.ax two directories down) - so
    // this also walks up from the entry file's own directory, one level at a time, looking for a
    // sibling "std" directory, stopping at the first one found (never collecting more than one -
    // a `std` directory further up an ancestor chain than the nearest one is never consulted).
    // Deliberately no other search path exists this phase (no CLI flag, no environment variable,
    // no install-relative path) - this is the narrowest extension that makes a `std/`-rooted
    // library layout actually reachable, not a general configurable search path.
    std::vector<std::filesystem::path> searchDirectories(const std::filesystem::path& entryDir)
    {
        std::vector<std::filesystem::path> dirs{entryDir};

        const std::filesystem::path absoluteEntryDir = std::filesystem::absolute(entryDir);
        for (std::filesystem::path dir = absoluteEntryDir;;)
        {
            const auto candidate = dir / "std";
            if (candidate != absoluteEntryDir && std::filesystem::is_directory(candidate))
            {
                dirs.push_back(candidate);
                break;
            }
            if (!dir.has_parent_path() || dir.parent_path() == dir)
            {
                break;
            }
            dir = dir.parent_path();
        }
        return dirs;
    }
} // namespace

Program loadProgram(const std::string& rootPathText)
{
    const std::filesystem::path rootPath(rootPathText);
    const std::filesystem::path entryDir =
        rootPath.has_parent_path() ? rootPath.parent_path() : std::filesystem::path(".");
    const std::vector<std::filesystem::path> searchDirs = searchDirectories(entryDir);

    Program rootProgram = parseFile(rootPath);

    // Every module the entry file's own top-level `use` declarations name (see
    // UnqualifiedCallResolver.hpp) - collected here, before `rootProgram` is consumed by
    // `mergeModule` below, since only the entry file's own top-level items are directly visible
    // at this point (a library module's own internal `use`s never contribute - matching this
    // feature's entry-file-only scope).
    std::vector<std::string> usedModules;
    std::vector<std::string> worklist;
    for (const auto& item : rootProgram.items)
    {
        if (const auto* useDecl = dynamic_cast<const UseDecl*>(item.get()))
        {
            worklist.push_back(useDecl->moduleName);
            usedModules.push_back(useDecl->moduleName);
        }
    }

    Program merged;
    mergeModule(merged, std::move(rootProgram), "");

    std::unordered_set<std::string> loaded;
    while (!worklist.empty())
    {
        const std::string wanted = worklist.back();
        worklist.pop_back();
        if (!loaded.insert(wanted).second)
        {
            continue;
        }

        bool found = false;
        for (const auto& searchDir : searchDirs)
        {
            if (!std::filesystem::is_directory(searchDir))
            {
                continue;
            }
            for (const auto& entry : std::filesystem::directory_iterator(searchDir))
            {
                if (entry.path().extension() != ".ax" ||
                    entry.path().filename() == rootPath.filename())
                {
                    continue;
                }
                Program candidate = parseFile(entry.path());
                if (candidate.moduleName != wanted)
                {
                    continue;
                }
                for (const auto& item : candidate.items)
                {
                    if (const auto* useDecl = dynamic_cast<const UseDecl*>(item.get()))
                    {
                        worklist.push_back(useDecl->moduleName);
                    }
                }
                mergeModule(merged, std::move(candidate), wanted);
                found = true;
                break;
            }
            if (found)
            {
                break;
            }
        }
        if (!found)
        {
            std::string searchedText;
            for (const auto& searchDir : searchDirs)
            {
                if (!searchedText.empty())
                {
                    searchedText += ", ";
                }
                searchedText += searchDir.string();
            }
            throw std::runtime_error("cannot find module '" + wanted + "' (searched " +
                                     searchedText + ")");
        }
    }

    // `use ModuleName` unqualified-call resolution (see UnqualifiedCallResolver.hpp) - must run
    // before monomorphizeGenerics below, so a bare generic call's own rewritten (now-qualified)
    // callee is what GenericMonomorphizer's own functionTemplatesByName lookup sees.
    resolveUnqualifiedCalls(merged, usedModules);
    // `TypeName<T>(args)` construction sugar (see UnqualifiedCallResolver.hpp's own doc comment)
    // - runs after the plain-function resolution above (so a real function sharing a struct's
    // own bare name already wins) and, unlike it, unconditionally (a purely local struct +
    // constructor needs no `use` at all).
    resolveUnqualifiedConstructorCalls(merged, usedModules);

    // Generic struct instantiations (see docs/language/0006-generics.md) - runs once, here,
    // after every module is merged into one flat Program and before any later pass ever sees
    // it, so TypeChecker/CapabilityChecker/RegionChecker/Interpreter/IrGenerator's own
    // structs_-registration loops pick up every synthesized concrete instantiation for free.
    monomorphizeGenerics(merged);

    return merged;
}
