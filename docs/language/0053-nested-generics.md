# Nested Generic Element Types: `elementKind` → `elementTypeName`

---

# Motivation

`docs/language/0052-optional.md`'s own "Known Imprecision" section documented
a real, freshly-discovered bug: `List<Optional<i32>>` silently miscompiled.
`Array`/`Slice`/`List`/`Stack`/`LinkedList`/`Deque`/`Queue`/`PriorityQueue`/
`Optional` all stored their single type parameter as a flat `TypeKind
elementKind` field - fine for a *leaf* element type (`i32`, a struct), but
`Optional`'s own payload type is itself a `TypeKind`-parameterized type,
which a single flat `TypeKind` can't carry. Reading it back
(`simpleType(objectType.elementKind, objectType.elementStructName)`, used
throughout method-call return-type resolution) reconstructed
`Optional<i32>` as `Optional<Bool>` - `TypeKind`'s own zero-valued
enumerator - a silent type-corruption bug, not a hypothetical one, found
while testing `List<Optional<i32>>.push(...)`.

`Map<K,V>`/`Set<T>`/`SortedMap<K,V>`/`SortedSet<T>` never had this problem:
they'd already been storing their own type parameter(s) as a canonical,
re-resolvable *string* (`elementTypeName`/`valueTypeName`) since
`docs/language/0034-maps-and-sets.md`'s own generic rewrite, specifically
because K/V needed to support arbitrary nesting from the start. This phase
moves the other nine kinds onto that same representation - not a new
mechanism, just wider use of one already proven correct.

---

# Design: One Representation, Not Two

**Why a string, not `std::shared_ptr<Type>`.** The "obvious" fix for a
flat `TypeKind` that can't express nesting is a real recursive node -
`std::shared_ptr<Type> elementType`. `docs/language/0031-arrays.md`
explicitly considered and rejected exactly this for `Array` originally:
`Type::operator==` is `= default`, and a defaulted comparison over a
`shared_ptr` member compares *pointer identity*, not the pointee's
contents - two independently-`resolveType`'d structurally-identical array
types would silently compare unequal, breaking every declared-type check,
call-argument check, and array-literal-unification check relying on
`Type::operator==`. A `std::string` sidesteps this entirely (`operator==`
on `std::string` is already structural), which is exactly why Map/Set
chose it over a recursive node in the first place - this phase just
recognized that reasoning generalizes to every other single-parameter
kind too, rather than re-litigating it.

**The `Type` struct** now has one `elementTypeName` field (shared by
`Array`/`Slice`/`List`/`Stack`/`LinkedList`/`Deque`/`Queue`/
`PriorityQueue`/`Optional`/`Set`, plus `Map`/`SortedMap`'s own key -
`valueTypeName` stays their own value type), and `arraySize` (still
`Array`-only - a compile-time element count is genuinely not a type
parameter, so it stays a separate field rather than being folded into the
string). `elementKind`/`elementStructName` are gone entirely - not
deprecated, removed, since nothing downstream (`IrGenerator`/Interpreter/
`LlvmIrEmitter`) ever touched `TypeChecker::Type` in the first place (see
below).

**What actually changed, mechanically:** every `arrayLikeType(TypeKind::X,
elementType.kind, elementType.structName, ...)` call became
`arrayLikeType(TypeKind::X, typeName(elementType), ...)` (`typeName(...)`
producing the same canonical string Map/Set's own construction already
relies on); every `simpleType(objectType.elementKind,
objectType.elementStructName)` read-back (used throughout method-return-
type resolution - `.push`/`.pop`/`.get`/`.peek`/indexing/`?`/`.unwrap_or`)
became `resolveType(objectType.elementTypeName)`. `arrayLikeType`'s own
signature dropped its `TypeKind elementKind, std::string
elementStructName` pair for a single `std::string elementTypeName`. Every
"nested array/slice/List/.../Optional element types are not supported"
rejection (ten near-identical throw blocks, one per kind, added
incrementally as each kind shipped) was deleted outright - there's no
structural reason left to reject nesting once the representation can
actually carry it; `PriorityQueue<T>`'s own `isOrderableKind` restriction
stayed, since that's a *semantic* restriction (not every type has a total
order), unrelated to nesting depth.

**`[elem;N]`'s own array-type string parsing needed one more fix.** Array
syntax parses its own two parts (`elem`, `N`) by locating the top-level
`;` in `"[elem;N]"` - previously `name.find(';')`, the *first* semicolon
in the string. That's wrong the instant `elem` can itself be a nested
array (`"[[i32;3];4]"`): the first `;` belongs to the *inner* array, not
the outer split point. Fixed with a `findTopLevelSemicolon` helper,
depth-tracking `<`/`[`...`>`/`]` exactly like `findTopLevelComma` (Map/
Set's own `"K,V"` splitter) already does for the identical class of
problem, one level removed.

**`Parser::parseTypeName`'s own array-syntax branch** previously accepted
only a single `Identifier` token for the element type (`expect(Identifier,
...)`), unable to parse `[Optional<i32>;2]`/`[List<i32>;3]` at the source
level at all, regardless of what `TypeChecker` could represent. Changed to
a full recursive `parseTypeName()` call - the same "full recursive parse,
not one token" shape `List<elem>()`'s own constructor parsing already
established, for the identical reason.

---

# Why `IrGenerator`/Interpreter/`LlvmIrEmitter` Needed *No* Changes

`TypeChecker::Type` is never referenced outside `TypeChecker.cpp`/`.hpp` -
confirmed directly (`grep`, zero hits in `compiler/ir/`, `compiler/
interpreter/`, `compiler/llvmir/`). Every downstream pass already re-
derives what it needs independently, from the AST or from canonical type-
name *strings* (`IrFunction::paramTypes`, `IrListNew::elementTypeName`,
etc.) - never from `TypeChecker`'s own internal `Type` struct, which
doesn't survive past the type-checking phase at all. `LlvmIrEmitter::
llvmType(...)` was *already* a fully recursive string parser (`"List<" +
llvmType(elementName) + "*, i32}*"`-shaped dispatch, calling itself for
whatever the element name resolves to) - it never had a "one level only"
restriction to begin with, so `List<List<i32>>`'s own LLVM representation
(`{i32, {i32,i32*,i32}**, i32}*` - a list of pointers to list headers) was
already correctly derivable the moment `TypeChecker` stopped rejecting the
type text. Same for the Interpreter's own `Value` (a `std::variant`
that's already recursive by construction - `ListInstance::elements` is a
`std::vector<Value>`, and a `Value` can already hold a
`shared_ptr<ListInstance>` as one of its own elements). This phase's
entire *runtime-representation* story was, in effect, already generically
correct - only `TypeChecker`'s own validation layer needed to stop
artificially rejecting what the rest of the pipeline could already handle.

---

# A Second Bug Found Along the Way: Nested-Collection Printing

Verifying `List<List<i32>>` end-to-end (not just type-checking it)
surfaced a second, genuinely different bug: printing one. Six pre-existing
print-dispatch sites (top-level bindings, struct fields, four collection-
element loops - the same six `docs/language/0052-optional.md` already
had to patch for `Optional<T>`) each fall back to "assume a nested *named
struct* pointer, call `@axea.print.<name>`" for any LLVM type they don't
otherwise recognize. `structNameFromPointerType` blindly strips a leading
`%` and trailing `*` - correct for a real struct (`%Point*`), silently
wrong for any collection type, whose own outer header is *always* an
*anonymous* struct (`{i32, i32*, i32}*` for `List<i32>`, `{i32, i32,
%axea.MapEntry.<id>**}*` for `Map<K,V>` - a named type only ever
appears *nested inside*, never as the outer pointer). Printing a
`List<List<i32>>` at the top level hit exactly this: `elementType` was
`{i32, i32*, i32}*` (a `List<i32>` itself), `structNameFromPointerType`
stripped the first character - `{`, not `%` - and produced `"i32,
i32*, i32}"` as a bogus "struct name," emitting a call to a function that
was never declared. Fixed with a new `isNamedStructPointerType` guard
(`type.front() == '%' && type.back() == '*'` - the one shape every
collection's own outer header structurally can't produce) checked before
every `structNameFromPointerType` call; anything that fails the guard now
throws a clear "not supported this phase" error instead of emitting
invalid IR. Construction, indexing, and every method call on a nested
collection all work fully - only *printing* one directly (at the top
level, in a struct field, or as a collection element) is scoped out, and
scoped out cleanly, not silently.

---

# Worked Example

```ax
xs: List<Optional<i32>> = List<Optional<i32>>()
a = xs.push(Some(1))
b = xs.push("bad".parse<i32>())
first = xs[0].unwrap_or(999)
second = xs[1].unwrap_or(999)

m: Map<i32, Optional<i32>> = Map<i32, Optional<i32>>()
c = m.set(1, Some(5))
d = m.set(2, "bad".parse<i32>())
mv = m.get(1).unwrap_or(0)

arr: [Optional<i32>; 2] = [Some(9), "x".parse<i32>()]
arrFirst = arr[0].unwrap_or(0)

deep() -> i32
{
    inner: List<i32> = List<i32>()
    e = inner.push(42)
    opt: Optional<List<i32>> = Some(inner)
    none: Optional<List<i32>> = None
    outer: List<Optional<List<i32>>> = List<Optional<List<i32>>>()
    f = outer.push(opt)
    g = outer.push(none)
    got = outer[0]
    fallback: List<i32> = List<i32>()
    return got.unwrap_or(fallback)[0]
}
result = deep()
```

`xs` prints `[Some(1), None]` (a `List<Optional<i32>>` itself is a
top-level binding of a list whose *elements* are `Optional<i32>` - already
supported, since `Optional<T>`'s own printing was built out in
`docs/language/0052-optional.md`; it's a `List<List<i32>>` binding
specifically that hits the known printing gap above). `result = 42`
confirms three levels of real nesting (`List<Optional<List<i32>>>`) round-
trip correctly through construction, `.push`, indexing, and
`.unwrap_or`. Every value here hand-verified byte-for-byte identical
across the interpreter, `ax llvm-ir | clang -x ir -O0`, and `-O1`.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **Printing a collection whose element type is itself a collection is
  not supported.** See the bug/fix section above - a clean, thrown error,
  not corrupted IR. Would need genuine recursive print codegen (a nested
  collection's own element-print step would itself need to emit a whole
  second bracket-wrapped loop) - a real sub-feature of its own, not built
  out this phase. `Optional<T>`'s own printing already covers `T ∈ {i32,
  i64, f64, bool}` (`docs/language/0052-optional.md`) and is unaffected
  by this restriction - only *collection-of-collection* printing hits it.
- **`None` still needs surrounding context to type-check**
  (`docs/language/0052-optional.md`'s own design) - `list.push(None)` (no
  declared-type/return-type context at that call site) still doesn't
  infer a type for `None`, regardless of nesting depth. Assign it to a
  declared-type local first (`x: Optional<T> = None`) and push that.
- **`Set<T>`/`SortedSet<T>`'s own element-type restrictions are
  unchanged** - hashability/orderability, not nesting depth. A
  `Set<List<i32>>` was already possible before this phase (List<T> is
  hashable when its own element type is) and still is; this phase didn't
  touch that logic at all, since Set/SortedSet already used the string-
  based representation.

---

# Guiding Rule

A "one level deep only" restriction that exists purely because the *data
structure* can't represent anything deeper is a representation bug wearing
a policy costume - the moment one part of the type system (Map/Set) has
already solved the general problem correctly, check whether every other
part quietly has the same limitation for the same reason, rather than
treating each occurrence as its own independent, permanent design
decision. And when fixing the representation, verify the *rest* of the
pipeline before declaring victory - `TypeChecker` accepting a type is
necessary, not sufficient; this phase's own second bug (printing) was
found only by actually running the now-legal programs through both
backends, not by reasoning about the type checker in isolation.
