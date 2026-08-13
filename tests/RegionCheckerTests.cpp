#include "TestFramework.hpp"

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

        TypeChecker typeChecker;
        typeChecker.check(program);

        CapabilityChecker capabilityChecker;
        capabilityChecker.check(program);

        RegionChecker regionChecker;
        regionChecker.check(program, capabilityChecker.effectiveCapabilities());
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
