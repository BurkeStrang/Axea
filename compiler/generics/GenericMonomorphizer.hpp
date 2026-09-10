#pragma once

#include "ast/Stmt.hpp"

// User-defined generics (docs/language/0006-generics.md) - generic structs and generic inherent
// impl/methods (`impl<T> Box<T> { ... }`, see docs/language/0006-generics.md's own
// generic-methods follow-up), explicit type arguments only (no inference; no generic top-level
// functions yet - only struct/impl-level type parameters).
//
// Mutates `program` in place: for every distinct concrete `Name<Arg1,...,ArgN>` instantiation
// textually referenced anywhere in the program, synthesizes a fully field-type-substituted,
// ordinary `StructDecl` (name mangled with '$' - "Box<i32>" -> "Box$i32", per
// docs/language/0006-generics.md's own Name Mangling section) and appends it to `program.items`,
// then does the same for any matching `impl<T> Box<T> { ... }` block's own methods (deep-cloned,
// substituted, mangled to e.g. "Box$i32.get", appended as plain top-level FunctionDecl items);
// rewrites every occurrence of the bracket-syntax reference to that struct's own final mangled
// name in place. Runs to a fixed point (`Box<Box<i32>>` resolves over two iterations).
//
// Must run - on a mutable `Program&` - before TypeChecker/CapabilityChecker/RegionChecker/
// Interpreter/IrGenerator ever see `program`: every one of those passes' own "loop over
// program.items, register any StructDecl by its exact name" code then picks up each synthesized
// instantiation for free, with zero further changes, because by the time they run it is
// byte-for-byte indistinguishable from an ordinary hand-written struct. `$` can never appear in a
// real Axea identifier (see Lexer's own isalpha/isalnum/'_' identifier rule), so a mangled name
// can never collide with user code, and never carries the '<'/'>'/',' characters that would
// otherwise be illegal in the bare LLVM identifiers IrGenerator/LlvmIrEmitter build from it.
//
// A built-in generic-shaped type (List<T>, Map<K,V>, Optional<T>, Result<T,E>, Shared<T>,
// slice<T>, and the other collection intrinsics already hardcoded in
// Parser::parseTypeNameAtom/TypeChecker::resolveType) is left completely untouched - those are
// never StructDecl-backed, so they're recognized by name and skipped rather than misdiagnosed as
// an unknown user generic.
void monomorphizeGenerics(Program& program);
