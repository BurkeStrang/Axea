#include "TestFramework.hpp"

#include "generics/GenericMonomorphizer.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/CapabilityChecker.hpp"
#include "sema/RegionChecker.hpp"
#include "sema/TypeChecker.hpp"

namespace
{
    // Mirrors the real pipeline (compiler/main.cpp): TypeChecker, then
    // CapabilityChecker, then RegionChecker - each stage's output feeds the
    // next, same as tests/CapabilityCheckerTests.cpp does for its own stage.
    void checkRegions(const std::string& source)
    {
        Lexer lexer(source);
        Parser parser(lexer.lex());
        auto program = parser.parseProgram();
        monomorphizeGenerics(program);

        TypeChecker typeChecker;
        typeChecker.check(program);

        CapabilityChecker capabilityChecker;
        capabilityChecker.check(program);

        RegionChecker regionChecker;
        regionChecker.check(program,
                            capabilityChecker.effectiveCapabilities(),
                            capabilityChecker.closureEffectiveCapabilities());
    }
} // namespace

TEST("RegionChecker accepts a freshly constructed owned struct literal being returned")
{
    checkRegions("struct Point { x: i32  y: i32 } "
                 "make(x: i32, y: i32) -> Point { return Point { x: x  y: y } } "
                 "p = make(1, 2)");
}

TEST("RegionChecker accepts a take parameter being returned directly")
{
    checkRegions("struct Packet { id: i32 } "
                 "consume(take packet: Packet) -> Packet { return packet } "
                 "p = Packet { id: 1 } "
                 "q = consume(p)");
}

TEST("RegionChecker accepts a primitive field extracted from a borrowed parameter and returned")
{
    checkRegions("struct User { name: str } "
                 "get_name(user: User) -> str { return user.name } "
                 "u = User { name: \"Burke\" } "
                 "n = get_name(u)");
}

TEST("RegionChecker accepts a struct literal built only from primitive fields of a borrowed "
     "parameter")
{
    checkRegions("struct Point { x: i32  y: i32 } "
                 "copy_point(p: Point) -> Point { return Point { x: p.x  y: p.y } } "
                 "p = Point { x: 1  y: 2 } "
                 "q = copy_point(p)");
}

TEST("RegionChecker accepts a returned value that came from another function's call result")
{
    checkRegions("struct Point { x: i32 } "
                 "make(x: i32) -> Point { return Point { x: x } } "
                 "wrap(x: i32) -> Point { return make(x) } "
                 "p = wrap(5)");
}

TEST("RegionChecker does not reject functions that don't return a struct, "
     "regardless of how they use their borrowed parameters")
{
    checkRegions("struct User { name: str  age: i32 } "
                 "birthday(user: User) -> i32 { user.age++  return user.age } "
                 "u = User { name: \"Burke\"  age: 35 } "
                 "x = birthday(u)");
}

TEST("RegionChecker rejects returning a borrowed struct parameter directly")
{
    const std::string source = "struct User { name: str } "
                               "get_ref(user: User) -> User { return user } "
                               "u = User { name: \"Burke\" } "
                               "x = get_ref(u)";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker rejects a returned struct literal containing a borrowed struct-typed field")
{
    const std::string source = "struct User { name: str } "
                               "struct Wrapper { inner: User } "
                               "wrap(user: User) -> Wrapper { return Wrapper { inner: user } } "
                               "u = User { name: \"Burke\" } "
                               "w = wrap(u)";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker rejects a borrowed parameter returned early inside a nested if")
{
    const std::string source = "struct User { name: str } "
                               "guard(user: User, flag: bool) -> User { "
                               "  if flag { return user } "
                               "  return User { name: \"fallback\" } "
                               "} "
                               "u = User { name: \"Burke\" } "
                               "x = guard(u, true)";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker rejects an if-expression when either branch could be borrowed")
{
    const std::string source = "struct User { name: str } "
                               "pick(user: User, flag: bool) -> User { "
                               "  return if flag { user } else { User { name: \"fallback\" } } "
                               "} "
                               "u = User { name: \"Burke\" } "
                               "x = pick(u, true)";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker rejects a borrowed struct parameter escaping via a loop's break value")
{
    const std::string source = "struct User { name: str } "
                               "badFind(u: User, flag: bool) -> User { "
                               "  return loop { "
                               "    if flag { break u } "
                               "  } "
                               "} "
                               "u = User { name: \"Burke\" } "
                               "x = badFind(u, true)";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker accepts an owned struct produced entirely within a loop's break value")
{
    checkRegions("struct Point { x: i32 } "
                 "make() -> Point { "
                 "  return loop { break Point { x: 1 } } "
                 "} "
                 "p = make()");
}

TEST("RegionChecker rejects returning a borrowed array parameter directly")
{
    const std::string source = "get_ref(values: [i32; 3]) -> [i32; 3] { return values } "
                               "x = get_ref([1, 2, 3])";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker accepts a take array parameter being returned directly")
{
    checkRegions("consume(take values: [i32; 3]) -> [i32; 3] { return values } "
                 "x = consume([1, 2, 3])");
}

TEST("RegionChecker accepts a primitive element read from a borrowed array parameter and returned")
{
    checkRegions("first(values: [i32; 3]) -> i32 { return values[0] } "
                 "x = first([1, 2, 3])");
}

// SortedMap<K,V> is a real, user-declared generic struct now (see
// docs/language/0040-sorted-maps.md's own "2026 Update") - `.get()` reaches it via the general
// struct method dispatch, unconditionally Region::Owned under the default rule (an accepted gap,
// matching Map<K,V>/Stack<T>'s own identical port precedent - see RegionChecker.cpp's own updated
// comment on this), so there is no dedicated SortedMap<K,V> aliasing-exception test left here
// either (mirrors Map<K,V>/Set<T>'s own identical, already-complete removal from this file); a
// borrowed SortedMap<K,V> parameter returned directly is already covered generically by "RegionChecker
// rejects returning a borrowed struct parameter directly" above - no SortedMap-specific version
// needed.

// SortedSet<T> is a real, user-declared generic struct now (see
// docs/language/0041-sorted-sets.md's own "2026 Update") - a borrowed SortedSet<T> parameter
// returned directly is already covered generically by "RegionChecker rejects returning a
// borrowed struct parameter directly" above - no SortedSet-specific version needed (mirrors
// SortedMap<K,V>'s own identical port precedent just above). This is the last of these
// collection-specific region tests: every collection docs/language/0029-collections.md
// originally scoped as a compiler intrinsic is now real Axea source.

TEST("RegionChecker rejects returning a borrowed String parameter directly")
{
    EXPECT_THROWS(checkRegions("leak(s: String) -> String { return s } "
                               "a = String(\"x\") "
                               "x = leak(a)"));
}

TEST("RegionChecker accepts a take String parameter being returned directly")
{
    checkRegions("consume(take s: String) -> String { return s } "
                 "a = String(\"x\") "
                 "x = consume(a)");
}

TEST("RegionChecker rejects returning a borrowed Buffer parameter directly")
{
    EXPECT_THROWS(checkRegions("leak(b: Buffer) -> Buffer { return b } "
                               "a = Buffer() "
                               "x = leak(a)"));
}

TEST("RegionChecker accepts a take Buffer parameter being returned directly")
{
    checkRegions("consume(take b: Buffer) -> Buffer { return b } "
                 "a = Buffer() "
                 "x = consume(a)");
}

TEST("RegionChecker accepts returning a fresh String from a borrowed Buffer's .finish()")
{
    checkRegions("done(b: Buffer) -> String { return b.finish() } "
                 "a = Buffer() "
                 "s = done(a)");
}

TEST("RegionChecker accepts a freshly constructed Buffer() being returned directly")
{
    checkRegions("build() -> Buffer { return Buffer() } "
                 "a = build()");
}

TEST("RegionChecker accepts returning a char literal or a char parameter directly - a plain "
     "value with nothing to borrow or alias, unlike every owned/reference-semantics type above")
{
    checkRegions("literal() -> char { return 'A' } "
                 "identity(c: char) -> char { return c } "
                 "a = literal() "
                 "b = identity(a)");
}

TEST("RegionChecker accepts returning a str slice taken directly from a borrowed str/String "
     "parameter - a slice always allocates a fresh, independently-owned buffer, never aliasing "
     "the borrowed source (see docs/language/0045-str-slicing.md)")
{
    checkRegions("firstFour(d: str) -> str { return d[..4] } "
                 "date = \"2026-08-18\" "
                 "a = firstFour(date) "
                 "sliceOfString(s: String) -> str { return s[0..2] } "
                 "s = String(\"Axea\") "
                 "b = sliceOfString(s)");
}

TEST("RegionChecker accepts returning a parse<T>() result taken directly from a borrowed str "
     "parameter - i32/bool are plain values with nothing to borrow or alias")
{
    checkRegions("toInt(d: str) -> Optional<i32> { return d.parse<i32>() } "
                 "a = toInt(\"42\")");
}

TEST("RegionChecker accepts enum variant construction and 'match' - a bare enum type name in "
     "EnumName.Variant(...)/EnumName.Variant position is never mistaken for a real bound "
     "variable and looked up in env (see docs/language/0064-enums.md) - this is the one real "
     "bug this phase's own worked-example verification found: RegionChecker's generic "
     "MethodCallExpr/FieldExpr handling recurses into `object` unconditionally, unlike "
     "CapabilityChecker's own generic path, which happens to be safe for an unbound name "
     "already")
{
    checkRegions("enum Shape { Circle(f64)  Point } "
                 "area(s: Shape) -> f64 { "
                 "  return match s { Circle(r) => r  Point => 0.0 } "
                 "} "
                 "c = Shape.Circle(5.0) "
                 "p = Shape.Point "
                 "x = area(c)");
}

TEST("RegionChecker accepts a union-typed parameter and 'match' on it - unlike a real enum's own "
     "EnumName.Variant construction syntax, a union value is only ever produced by implicit "
     "wrapping (see docs/language/0065-unions.md), never a bare-type-name method/field access, so "
     "the fix above doesn't need extending for unions specifically")
{
    checkRegions("f(x: i32 | str) -> str { "
                 "  return match x { i32(n) => \"number\"  str(s) => \"string\" } "
                 "} "
                 "y = f(5)");
}

TEST("RegionChecker accepts returning Ok(x)/Err(e) built from a borrowed parameter's own field, "
     "and '?' propagating through a Result<T,E>-returning function (see "
     "docs/language/0063-result.md)")
{
    checkRegions("divide(a: i32, b: i32) -> Result<i32, i32> { "
                 "  if b == 0 { return Err(a) } "
                 "  return Ok(a / b) "
                 "} "
                 "sumTwo(a: i32, b: i32, c: i32, d: i32) -> Result<i32, i32> { "
                 "  x = divide(a, b)? "
                 "  y = divide(c, d)? "
                 "  return Ok(x + y) "
                 "} "
                 "z = sumTwo(10, 2, 20, 4)");
}

TEST("RegionChecker accepts an extern call through .to_cstr() on a borrowed str parameter")
{
    checkRegions("extern c puts(text: cstr) "
                 "greet(name: str) -> i32 { called = puts(name.to_cstr())  return 1 } "
                 "a = greet(\"hi\")");
}

TEST("RegionChecker propagates struct-type awareness through an inherent method call's own "
     "return value, so a chained field access on the result resolves correctly (see "
     "docs/language/0006-generics.md's own generic-methods follow-up)")
{
    checkRegions("struct Point { x: i32 } "
                "struct Box { value: Point } "
                "impl Box { getX(self) -> i32 { return self.value.x } } "
                "compute(b: Box) -> i32 { n = b.getX() return n } "
                "bx = Box { value: Point { x: 5 } } "
                "r = compute(bx)");
}

TEST("RegionChecker rejects a borrowed self's own field being moved out through an inherent "
     "method's return, the same way it already rejects any other borrow leak")
{
    EXPECT_THROWS(checkRegions("struct Point { x: i32 } "
                               "struct Box { value: Point } "
                               "impl Box { getValue(self) -> Point { return self.value } } "
                               "bx = Box { value: Point { x: 5 } } "
                               "v = bx.getValue()"));
}

TEST("RegionChecker accepts print/write called with a borrowed parameter's fields - the builtin "
     "call arguments are read-only, same as any other read-only use")
{
    checkRegions("struct Point { x: i32 } "
                 "show(p: Point) -> i32 { print(p.x) return 1 } "
                 "a = Point { x: 1 } "
                 "x = show(a)");
}

TEST("RegionChecker treats an interpolated string literal's result as Owned, matching the "
     "InterpolatedStringExpr's always-owned String typing")
{
    checkRegions("struct Packet { id: i32 } "
                 "consume(take p: Packet) -> Packet { return p } "
                 "greet() -> Packet { "
                 "  a = Packet { id: 1 } "
                 "  s = \"packet {a.id}\" "
                 "  return consume(a) "
                 "} "
                 "x = greet()");
}

TEST("RegionChecker treats a .join() of a borrowed Array parameter as Owned - always allocates a "
     "fresh String, never aliasing the source")
{
    checkRegions("describe(nums: [i32; 4]) -> String { return nums.join(\",\") } "
                 "x = describe([1, 2, 3, 4])");
}

TEST("RegionChecker rejects a struct-typed closure parameter (borrowed, since its own body only "
     "reads it) being returned directly out of the closure body - the same aliasing rejection a "
     "real function's own borrowed struct param already gets (see "
     "docs/language/0067-closures.md's own closureCapabilities_/closureRegions_)")
{
    EXPECT_THROWS(
        checkRegions("struct Point { x: i32  y: i32 } "
                     "run() -> Point { "
                     "  identity: fn(Point) -> Point = fn(p: Point) -> Point { return p } "
                     "  return identity(Point { x: 1, y: 2 }) "
                     "} "
                     "y = run()"));
}

TEST("RegionChecker accepts a `take`-declared struct-typed closure parameter being returned "
     "directly - ownership genuinely transfers, so this isn't an aliasing violation")
{
    checkRegions("struct Point { x: i32  y: i32 } "
                 "run() -> Point { "
                 "  identity: fn(Point) -> Point = fn(take p: Point) -> Point { return p } "
                 "  return identity(Point { x: 1, y: 2 }) "
                 "} "
                 "y = run()");
}

TEST("RegionChecker treats a primitive-typed generic struct field as owned, freely returnable "
     "from a borrowed parameter - substitution feeds move-tracking exactly like a plain field")
{
    checkRegions("struct Box<T> { value: T } "
                 "get_value(box: Box<i32>) -> i32 { return box.value } "
                 "b = Box<i32> { value: 5 } "
                 "n = get_value(b)");
}

TEST("RegionChecker rejects returning a struct-typed generic field extracted from a borrowed "
     "parameter, matching a plain struct field's identical borrowed-field rule")
{
    const std::string source = "struct Point { x: i32  y: i32 } "
                               "struct Box<T> { value: T } "
                               "get_point(box: Box<Point>) -> Point { return box.value } "
                               "b = Box<Point> { value: Point { x: 1  y: 2 } } "
                               "p = get_point(b)";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker treats a '*T' parameter as always Owned, regardless of declared capability - "
     "returning it directly is accepted, matching the identical existing rule for any other "
     "primitive parameter (see docs/language/0019-unsafe.md)")
{
    checkRegions("f(ptr: *i32) -> *i32 { return ptr }");
}
