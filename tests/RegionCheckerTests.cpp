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
    checkRegions(R"AXEA(struct Point
{
    i32 x
    i32 y
} Point make(i32 x, i32 y)
{ return Point { x: x  y: y } } p = make(1, 2))AXEA");
}

TEST("RegionChecker accepts a take parameter being returned directly")
{
    checkRegions(R"AXEA(struct Packet
{
    i32 id
} Packet consume(take Packet packet)
{ return packet } p = Packet { id: 1 } q = consume(p))AXEA");
}

TEST("RegionChecker accepts a primitive field extracted from a borrowed parameter and returned")
{
    checkRegions(R"AXEA(struct User
{
    str name
} str get_name(User user)
{ return user.name } u = User { name: "Burke" } n = get_name(u))AXEA");
}

TEST("RegionChecker accepts a struct literal built only from primitive fields of a borrowed "
     "parameter")
{
    checkRegions(R"AXEA(struct Point
{
    i32 x
    i32 y
} Point copy_point(Point p)
{ return Point { x: p.x  y: p.y } } p = Point { x: 1  y: 2 } q = copy_point(p))AXEA");
}

TEST("RegionChecker accepts a returned value that came from another function's call result")
{
    checkRegions(R"AXEA(struct Point
{
    i32 x
} Point make(i32 x)
{ return Point { x: x } } Point wrap(i32 x)
{ return make(x) } p = wrap(5))AXEA");
}

TEST("RegionChecker does not reject functions that don't return a struct, "
     "regardless of how they use their borrowed parameters")
{
    checkRegions(R"AXEA(struct User
{
    str name
    i32 age
} i32 birthday(User user)
{ user.age++  return user.age } u = User { name: "Burke"  age: 35 } x = birthday(u))AXEA");
}

TEST("RegionChecker rejects returning a borrowed struct parameter directly")
{
    const std::string source = R"AXEA(struct User
{
    str name
} User get_ref(User user)
{ return user } u = User { name: "Burke" } x = get_ref(u))AXEA";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker rejects a returned struct literal containing a borrowed struct-typed field")
{
    const std::string source = R"AXEA(struct User
{
    str name
} struct Wrapper
{
    User inner
} Wrapper wrap(User user)
{ return Wrapper { inner: user } } u = User { name: "Burke" } w = wrap(u))AXEA";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker rejects a borrowed parameter returned early inside a nested if")
{
    const std::string source = R"AXEA(struct User
{
    str name
} User guard(User user, bool flag)
{   if flag { return user }   return User { name: "fallback" } } u = User { name: "Burke" } x = guard(u, true))AXEA";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker rejects an if-expression when either branch could be borrowed")
{
    const std::string source = R"AXEA(struct User
{
    str name
} User pick(User user, bool flag)
{   return if flag { user } else { User { name: "fallback" } } } u = User { name: "Burke" } x = pick(u, true))AXEA";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker rejects a borrowed struct parameter escaping via a loop's break value")
{
    const std::string source = R"AXEA(struct User
{
    str name
} User badFind(User u, bool flag)
{   return loop {     if flag { break u }   } } u = User { name: "Burke" } x = badFind(u, true))AXEA";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker accepts an owned struct produced entirely within a loop's break value")
{
    checkRegions(R"AXEA(struct Point
{
    i32 x
} Point make()
{   return loop { break Point { x: 1 } } } p = make())AXEA");
}

TEST("RegionChecker rejects returning a borrowed array parameter directly")
{
    const std::string source = R"AXEA([i32; 3] get_ref([i32; 3] values)
{ return values } x = get_ref([1, 2, 3]))AXEA";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker accepts a take array parameter being returned directly")
{
    checkRegions(R"AXEA([i32; 3] consume(take [i32; 3] values)
{ return values } x = consume([1, 2, 3]))AXEA");
}

TEST("RegionChecker accepts a primitive element read from a borrowed array parameter and returned")
{
    checkRegions(R"AXEA(i32 first([i32; 3] values)
{ return values[0] } x = first([1, 2, 3]))AXEA");
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
    EXPECT_THROWS(checkRegions(R"AXEA(String leak(String s)
{ return s } a = String("x") x = leak(a))AXEA"));
}

TEST("RegionChecker accepts a take String parameter being returned directly")
{
    checkRegions(R"AXEA(String consume(take String s)
{ return s } a = String("x") x = consume(a))AXEA");
}

TEST("RegionChecker rejects returning a borrowed Buffer parameter directly")
{
    EXPECT_THROWS(checkRegions(R"AXEA(Buffer leak(Buffer b)
{ return b } a = Buffer() x = leak(a))AXEA"));
}

TEST("RegionChecker accepts a take Buffer parameter being returned directly")
{
    checkRegions(R"AXEA(Buffer consume(take Buffer b)
{ return b } a = Buffer() x = consume(a))AXEA");
}

TEST("RegionChecker accepts returning a fresh String from a borrowed Buffer's .finish()")
{
    checkRegions(R"AXEA(String done(Buffer b)
{ return b.finish() } a = Buffer() s = done(a))AXEA");
}

TEST("RegionChecker accepts a freshly constructed Buffer() being returned directly")
{
    checkRegions(R"AXEA(Buffer build()
{ return Buffer() } a = build())AXEA");
}

TEST("RegionChecker accepts returning a char literal or a char parameter directly - a plain "
     "value with nothing to borrow or alias, unlike every owned/reference-semantics type above")
{
    checkRegions(R"AXEA(char literal()
{ return 'A' } char identity(char c)
{ return c } a = literal() b = identity(a))AXEA");
}

TEST("RegionChecker accepts returning a str slice taken directly from a borrowed str/String "
     "parameter - a slice always allocates a fresh, independently-owned buffer, never aliasing "
     "the borrowed source (see docs/language/0045-str-slicing.md)")
{
    checkRegions(R"AXEA(str firstFour(str d)
{ return d[..4] } date = "2026-08-18" a = firstFour(date) sliceOfString(s: String) -> str { return s[0..2] } s = String("Axea") b = sliceOfString(s))AXEA");
}

TEST("RegionChecker accepts returning a parse<T>() result taken directly from a borrowed str "
     "parameter - i32/bool are plain values with nothing to borrow or alias")
{
    checkRegions(R"AXEA(Optional<i32> toInt(str d)
{ return d.parse<i32>() } a = toInt("42"))AXEA");
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
    checkRegions(R"AXEA(str f(i32 | str x)
{   return match x { i32(n) => "number"  str(s) => "string" } } y = f(5))AXEA");
}

TEST("RegionChecker accepts returning Ok(x)/Err(e) built from a borrowed parameter's own field, "
     "and '?' propagating through a Result<T,E>-returning function (see "
     "docs/language/0063-result.md)")
{
    checkRegions(R"AXEA(Result<i32, i32> divide(i32 a, i32 b)
{   if b == 0 { return Err(a) }   return Ok(a / b) } Result<i32, i32> sumTwo(i32 a, i32 b, i32 c, i32 d)
{   x = divide(a, b)?   y = divide(c, d)?   return Ok(x + y) } z = sumTwo(10, 2, 20, 4))AXEA");
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
    checkRegions(R"AXEA(struct Point
{
    i32 x
} struct Box
{
    Point value

    i32 getX(self)
    { return self.value.x }
} i32 compute(Box b)
{ n = b.getX() return n } bx = Box { value: Point { x: 5 } } r = compute(bx))AXEA");
}

TEST("RegionChecker rejects a borrowed self's own field being moved out through an inherent "
     "method's return, the same way it already rejects any other borrow leak")
{
    EXPECT_THROWS(checkRegions(R"AXEA(struct Point
{
    i32 x
} struct Box
{
    Point value

    Point getValue(self)
    { return self.value }
} bx = Box { value: Point { x: 5 } } v = bx.getValue())AXEA"));
}

TEST("RegionChecker accepts print/write called with a borrowed parameter's fields - the builtin "
     "call arguments are read-only, same as any other read-only use")
{
    checkRegions(R"AXEA(struct Point
{
    i32 x
} i32 show(Point p)
{ print(p.x) return 1 } a = Point { x: 1 } x = show(a))AXEA");
}

TEST("RegionChecker treats an interpolated string literal's result as Owned, matching the "
     "InterpolatedStringExpr's always-owned String typing")
{
    checkRegions(R"AXEA(struct Packet
{
    i32 id
} Packet consume(take Packet p)
{ return p } Packet greet()
{   a = Packet { id: 1 }   s = "packet {a.id}"   return consume(a) } x = greet())AXEA");
}

TEST("RegionChecker treats a .join() of a borrowed Array parameter as Owned - always allocates a "
     "fresh String, never aliasing the source")
{
    checkRegions(R"AXEA(String describe([i32; 4] nums)
{ return nums.join(",") } x = describe([1, 2, 3, 4]))AXEA");
}

TEST("RegionChecker rejects a struct-typed closure parameter (borrowed, since its own body only "
     "reads it) being returned directly out of the closure body - the same aliasing rejection a "
     "real function's own borrowed struct param already gets (see "
     "docs/language/0067-closures.md's own closureCapabilities_/closureRegions_)")
{
    EXPECT_THROWS(
        checkRegions(R"AXEA(struct Point
{
    i32 x
    i32 y
} Point run()
{   identity: fn(Point) -> Point = fn(p: Point) -> Point { return p }   return identity(Point { x: 1, y: 2 }) } y = run())AXEA"));
}

TEST("RegionChecker accepts a `take`-declared struct-typed closure parameter being returned "
     "directly - ownership genuinely transfers, so this isn't an aliasing violation")
{
    checkRegions(R"AXEA(struct Point
{
    i32 x
    i32 y
} Point run()
{   identity: fn(Point) -> Point = fn(take p: Point) -> Point { return p }   return identity(Point { x: 1, y: 2 }) } y = run())AXEA");
}

TEST("RegionChecker treats a primitive-typed generic struct field as owned, freely returnable "
     "from a borrowed parameter - substitution feeds move-tracking exactly like a plain field")
{
    checkRegions(R"AXEA(struct Box<T>
{
    T value
} i32 get_value(Box<i32> box)
{ return box.value } b = Box<i32> { value: 5 } n = get_value(b))AXEA");
}

TEST("RegionChecker rejects returning a struct-typed generic field extracted from a borrowed "
     "parameter, matching a plain struct field's identical borrowed-field rule")
{
    const std::string source = R"AXEA(struct Point
{
    i32 x
    i32 y
} struct Box<T>
{
    T value
} Point get_point(Box<Point> box)
{ return box.value } b = Box<Point> { value: Point { x: 1  y: 2 } } p = get_point(b))AXEA";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker treats a '*T' parameter as always Owned, regardless of declared capability - "
     "returning it directly is accepted, matching the identical existing rule for any other "
     "primitive parameter (see docs/language/0019-unsafe.md)")
{
    checkRegions(R"AXEA(*i32 f(*i32 ptr)
{ return ptr })AXEA");
}
