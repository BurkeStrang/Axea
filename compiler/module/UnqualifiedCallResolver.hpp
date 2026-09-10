#pragma once

#include "ast/Stmt.hpp"

#include <string>
#include <vector>

// `use ModuleName` (see docs/language/0066-modules.md's own follow-up) - makes a used module's
// own public top-level functions (plain and generic) callable unqualified, e.g. `newList<i32>()`
// instead of `collections.newList<i32>()`, whenever the bare name doesn't collide with anything
// else already in scope. Entry-file only: `usedModules` is the set of module names named by a
// `use` declaration directly in the entry file's own top-level items (never a module file's own
// internal `use`s) - see ModuleLoader::loadProgram, which collects this before `rootProgram` is
// consumed by `mergeModule`.
//
// Must run on the fully merged Program, after every module's own functions are already
// name-qualified ("module.name") by ModuleLoader::mergeModule, and before monomorphizeGenerics
// runs - so GenericMonomorphizer's own functionTemplatesByName lookup (keyed on the qualified
// name) and every later pass's own functions_ map both see the already-rewritten callee text,
// with zero further changes needed anywhere else in the compiler.
//
// Resolution rules:
// - A bare call whose callee already contains '.' (already qualified), or matches an
//   entry-file-declared top-level function's own bare name, is left untouched - a local
//   declaration always silently wins over any same-named import, never an error.
// - Otherwise, if the bare callee matches exactly one used module's own public function, that
//   call's `callee` is rewritten in place to the qualified name.
// - If it matches 2+ used modules' own public functions, throws - but only for a bare name that
//   is actually called unqualified somewhere; two used modules silently sharing an unused bare
//   name is never an error (lazy, Rust-`use`-glob-style ambiguity).
// - A bare callee matching zero candidates is left untouched entirely - the ordinary "unknown
//   function" error surfaces normally, downstream, unchanged.
//
// `ExternDecl` participation is out of scope for this phase - only ordinary (and generic)
// `FunctionDecl`s are considered.
void resolveUnqualifiedCalls(Program& merged, const std::vector<std::string>& usedModules);

// `TypeName<T>(args)`/`TypeName(args)` construction sugar - lets a bare struct name used in call
// position (e.g. `List<i32>()`, or `Point(1, 2)` for a non-generic struct) stand in for a call to
// that struct's own conventionally-named constructor, `"new" + TypeName` (e.g. `newList<i32>()`),
// exactly the naming convention every real (non-intrinsic) collection in std/collections.ax
// already follows. `TypeName` itself is never a valid callee on its own - a struct name and a
// function name are never interchangeable anywhere else in this language - so this exists purely
// to make the common "call the type's own constructor" case as terse as calling any other
// function, the same ergonomic goal resolveUnqualifiedCalls above already serves for plain
// functions.
//
// Must run after resolveUnqualifiedCalls above (so a real function that happens to share a
// struct's own bare name, or an ordinary bare function call, is already resolved/left alone by
// the time this runs) and before monomorphizeGenerics, for the same reasons.
//
// Resolution rules, mirroring resolveUnqualifiedCalls' own shape exactly, with one difference:
// this runs even when `usedModules` is empty, since a purely local struct + local constructor
// (no `use` involved at all) still needs its callee rewritten - a bare struct name is never
// itself a valid callee, unlike a bare local function call, which already works with no rewrite.
// - A callee already qualified, not a known struct name, or already resolved to a real local
//   function's own bare name (by name collision) is left untouched.
// - Otherwise, `"new" + TypeName` is resolved exactly like resolveUnqualifiedCalls resolves any
//   other bare name: a local top-level function of that name always wins; otherwise exactly one
//   used module's own public function of that name resolves it; 2+ throws (same ambiguous-call
//   error shape); zero leaves the call untouched (the ordinary "not a known generic function" /
//   unknown-callee error surfaces downstream unchanged).
void resolveUnqualifiedConstructorCalls(Program& merged,
                                        const std::vector<std::string>& usedModules);
