#include "TestFramework.hpp"

#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/TypeChecker.hpp"

namespace
{
    void check(const std::string& source)
    {
        Lexer lexer(source);
        Parser parser(lexer.lex());
        auto program = parser.parseProgram();

        TypeChecker checker;
        checker.check(program);
    }
} // namespace

TEST("TypeChecker accepts a well-typed program")
{
    const std::string source = "struct Point { x: i32  y: i32 } "
                               "square(n: i32) -> i32 { return n * n } "
                               "p = Point { x: 1  y: 2 } "
                               "x = square(p.x) + p.y";
    check(source);
}

TEST("TypeChecker rejects wrong argument count")
{
    EXPECT_THROWS(check("f(a: i32, b: i32) -> i32 { return a + b }  x = f(1)"));
}

TEST("TypeChecker rejects wrong argument type")
{
    EXPECT_THROWS(check(R"(f(a: i32) -> i32 { return a }  x = f("oops"))"));
}

TEST("TypeChecker rejects a call to an undefined function")
{
    EXPECT_THROWS(check("x = missing(1)"));
}

TEST("TypeChecker rejects construction of an undefined struct")
{
    EXPECT_THROWS(check("x = Missing { a: 1 }"));
}

TEST("TypeChecker rejects access to an undefined field")
{
    EXPECT_THROWS(check("struct Point { x: i32 }  p = Point { x: 1 }  y = p.missing"));
}

TEST("TypeChecker rejects a struct literal with a missing field")
{
    EXPECT_THROWS(check("struct Point { x: i32  y: i32 }  p = Point { x: 1 }"));
}

TEST("TypeChecker rejects a struct literal field with the wrong type")
{
    EXPECT_THROWS(check(R"(struct Point { x: i32 }  p = Point { x: "oops" })"));
}

TEST("TypeChecker rejects arithmetic on non-integer operands")
{
    EXPECT_THROWS(check(R"(x = 1 + "oops")"));
}

TEST("TypeChecker rejects equality between incompatible types")
{
    EXPECT_THROWS(check(R"(x = 1 == "oops")"));
}

TEST("TypeChecker rejects a non-boolean if condition")
{
    EXPECT_THROWS(check("x = if 1 { 1 } else { 2 }"));
}

TEST("TypeChecker rejects mismatched if/else branch types")
{
    EXPECT_THROWS(check(R"(x = if true { 1 } else { "oops" })"));
}

TEST("TypeChecker rejects mismatched branch types across an else-if chain")
{
    EXPECT_THROWS(check(R"(x = if true { 1 } else if false { "oops" } else { 3 })"));
}

TEST("TypeChecker rejects an unsupported type annotation")
{
    EXPECT_THROWS(check("x: u16 = 1"));
}

TEST("TypeChecker rejects a declared type that does not match the initializer")
{
    EXPECT_THROWS(check(R"(x: i32 = "oops")"));
}

TEST("TypeChecker rejects a return value that does not match the declared return type")
{
    EXPECT_THROWS(check(R"(f() -> i32 { return "oops" })"));
}

TEST("TypeChecker rejects a value-returning function whose body never explicitly returns")
{
    EXPECT_THROWS(check("f() -> i32 { 1 }"));
}

TEST("TypeChecker allows a unit-returning function to fall off the end past a discarded expression")
{
    // Only value-producing functions must explicitly `return`
    // (docs/language/0027-explicit-return.md) - a unit function can still
    // fall off the end, and any trailing expression is simply discarded.
    check("f() { 1 }");
}

TEST("TypeChecker rejects return used outside a function")
{
    EXPECT_THROWS(check("x = if true { return 1 } else { 2 }"));
}

TEST("TypeChecker rejects a return value that does not match the function's return type")
{
    EXPECT_THROWS(check(R"(f() -> i32 { if true { return "oops" } 1 })"));
}

TEST("TypeChecker accepts a function whose entire body is an if/else where both branches return")
{
    // The exact shape docs/language/0027-explicit-return.md's whole change
    // is meant to make possible: neither branch produces a block-result
    // value (both just `return`), so this previously failed to type-check
    // under implicit-return semantics (the if-expression's own inferred
    // type was unit, mismatching the declared i32).
    check("sign(x: i32) -> i32 { if x < 0 { return 0 - 1 } else { return 1 } }");
}

TEST("TypeChecker rejects an if/else where only one branch returns and the other falls through")
{
    EXPECT_THROWS(check("f(x: i32) -> i32 { if x < 0 { return 0 - 1 } else { 1 } }"));
}

TEST("TypeChecker rejects a non-bool while condition")
{
    EXPECT_THROWS(check("f() { while 1 { } }"));
}

TEST("TypeChecker accepts a loop typed by its break values")
{
    check("f() -> i32 { return loop { break 1 } }");
}

TEST("TypeChecker treats a loop with no break as unit")
{
    // Documented imprecision (docs/language/0028-loops.md): a genuinely
    // infinite loop with no break is really `never`, but TypeKind::Never
    // has no checking logic wired up anywhere in this codebase.
    EXPECT_THROWS(check("f() -> i32 { return loop { 1 } }"));
}

TEST("TypeChecker rejects mismatched break value types in the same loop")
{
    EXPECT_THROWS(check(
        R"(f(flag: bool) -> i32 { return loop { if flag { break 1 } else { break "oops" } } })"));
}

TEST("TypeChecker rejects a break with a value inside while")
{
    EXPECT_THROWS(check("f() { while true { break 1 } }"));
}

TEST("TypeChecker allows a bare break inside while")
{
    check("f() { while true { break } }");
}

TEST("TypeChecker rejects break used outside a loop")
{
    EXPECT_THROWS(check("f() { break }"));
}

TEST("TypeChecker rejects continue used outside a loop")
{
    EXPECT_THROWS(check("f() { continue }"));
}

TEST("TypeChecker scopes break/continue validity to the innermost loop, correctly nested")
{
    check("f() { while true { while true { break } continue } }");
}

TEST("TypeChecker accepts a well-typed array literal, indexing, .length, and index-assignment")
{
    check("f(values: [i32; 3]) -> i32 { "
          "  values[0] = 99 "
          "  return values[0] + values.length "
          "} "
          "x = f([1, 2, 3])");
}

TEST("TypeChecker rejects an array literal with mismatched element types")
{
    EXPECT_THROWS(check(R"(x = [1, true, 3])"));
}

TEST("TypeChecker rejects an empty array literal with no type annotation to infer from")
{
    EXPECT_THROWS(check("x = []"));
}

TEST("TypeChecker rejects an array literal that does not match its declared type")
{
    EXPECT_THROWS(check(R"(x: [i32; 3] = ["a", "b", "c"])"));
}

TEST("TypeChecker rejects a compile-time out-of-range literal index")
{
    EXPECT_THROWS(check("x: [i32; 3] = [1, 2, 3]  y = x[5]"));
}

TEST("TypeChecker rejects a literal index equal to the array size (exclusive upper bound)")
{
    EXPECT_THROWS(check("x: [i32; 3] = [1, 2, 3]  y = x[3]"));
}

TEST("TypeChecker rejects indexing into a non-array type")
{
    EXPECT_THROWS(check("x = 5  y = x[0]"));
}

TEST("TypeChecker rejects a non-i32 index")
{
    EXPECT_THROWS(check("x: [i32; 3] = [1, 2, 3]  y = x[true]"));
}

TEST("TypeChecker types .length as i32")
{
    check("x: [i32; 3] = [1, 2, 3]  y: i32 = x.length");
}

TEST("TypeChecker rejects an unknown field access on an array other than length")
{
    EXPECT_THROWS(check("x: [i32; 3] = [1, 2, 3]  y = x.size"));
}

TEST("TypeChecker rejects an index-assignment whose value does not match the element type")
{
    EXPECT_THROWS(check(R"(f(values: [i32; 3]) { values[0] = "oops" })"));
}

TEST("TypeChecker accepts an array of any size for a slice<T> parameter")
{
    check("sum(values: slice<i32>) -> i32 { return values[0] } "
          "a = sum([1, 2, 3]) "
          "b = sum([1, 2, 3, 4, 5])");
}

TEST("TypeChecker rejects an element-type mismatch when converting an array to a slice parameter")
{
    EXPECT_THROWS(check(R"(f(values: slice<i32>) -> i32 { return values[0] }  x = f(["a", "b"]))"));
}

TEST("TypeChecker accepts forwarding an existing slice to another slice parameter")
{
    check("helper(values: slice<i32>) -> i32 { return values[0] } "
          "wrapper(values: slice<i32>) -> i32 { return helper(values) } "
          "x = wrapper([1, 2, 3])");
}

TEST("TypeChecker rejects slice<T> as a function return type")
{
    EXPECT_THROWS(check("f(values: slice<i32>) -> slice<i32> { return values }"));
}

TEST("TypeChecker rejects slice<T> as a local variable's declared type")
{
    EXPECT_THROWS(check("f(values: slice<i32>) -> i32 { x: slice<i32> = values  return x[0] }"));
}

TEST("TypeChecker rejects slice<T> as a struct field type")
{
    EXPECT_THROWS(check("struct Wrapper { values: slice<i32> }"));
}

TEST("TypeChecker allows indexing, .length, index-assignment, and for-in on a slice parameter")
{
    check("f(values: slice<i32>) -> i32 { "
          "  values[0] = 99 "
          "  total = 0 "
          "  for v in values { total = total + v } "
          "  return total + values[0] + values.length "
          "} "
          "x = f([1, 2, 3])");
}

TEST("TypeChecker accepts push/pop/indexing/.length/for-in on a List<T>")
{
    check("f() -> i32 { "
          "  numbers = List<i32>() "
          "  numbers.push(4) "
          "  numbers.push(5) "
          "  last = numbers.pop() "
          "  numbers[0] = 99 "
          "  total = 0 "
          "  for v in numbers { total = total + v } "
          "  return total + numbers[0] + numbers.length + last "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects 'push' with the wrong element type")
{
    EXPECT_THROWS(check("f() { numbers = List<i32>()  numbers.push(true) }"));
}

TEST("TypeChecker rejects 'push' with the wrong argument count")
{
    EXPECT_THROWS(check("f() { numbers = List<i32>()  numbers.push(1, 2) }"));
}

TEST("TypeChecker rejects 'pop' with arguments")
{
    EXPECT_THROWS(check("f() -> i32 { numbers = List<i32>()  return numbers.pop(1) }"));
}

TEST("TypeChecker rejects an unknown method")
{
    EXPECT_THROWS(check("f() { numbers = List<i32>()  numbers.size() }"));
}

TEST("TypeChecker rejects a method call on a non-List value")
{
    EXPECT_THROWS(check("f() { x = 5  x.push(1) }"));
}

TEST("TypeChecker accepts List<T> as a parameter, return type, and local declared type")
{
    check("build() -> List<i32> { "
          "  x: List<i32> = List<i32>() "
          "  return x "
          "} "
          "use(numbers: List<i32>) -> i32 { return numbers.length } "
          "n = build() "
          "y = use(n)");
}

TEST("TypeChecker rejects List<T> as a struct field type")
{
    EXPECT_THROWS(check("struct Wrapper { items: List<i32> }"));
}

TEST("TypeChecker accepts push/pop/peek/.length on a Stack<T>")
{
    check("f() -> i32 { "
          "  s = Stack<i32>() "
          "  s.push(4) "
          "  s.push(5) "
          "  top = s.peek() "
          "  last = s.pop() "
          "  return top + last + s.length "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects 'push' with the wrong element type on a Stack<T>")
{
    EXPECT_THROWS(check("f() { s = Stack<i32>()  s.push(true) }"));
}

TEST("TypeChecker rejects 'peek' with arguments")
{
    EXPECT_THROWS(check("f() -> i32 { s = Stack<i32>()  return s.peek(1) }"));
}

TEST("TypeChecker rejects an unknown method on a Stack<T>")
{
    EXPECT_THROWS(check("f() { s = Stack<i32>()  s.size() }"));
}

TEST("TypeChecker accepts Stack<T> as a parameter, return type, and local declared type")
{
    check("build() -> Stack<i32> { "
          "  x: Stack<i32> = Stack<i32>() "
          "  return x "
          "} "
          "use(s: Stack<i32>) -> i32 { return s.length } "
          "n = build() "
          "y = use(n)");
}

TEST("TypeChecker rejects Stack<T> as a struct field type")
{
    EXPECT_THROWS(check("struct Wrapper { items: Stack<i32> }"));
}

TEST("TypeChecker accepts set/get/contains/remove/.length on a Map<i32,i32>")
{
    check("f() -> i32 { "
          "  m = Map<i32,i32>() "
          "  m.set(1, 100) "
          "  m.set(1, 999) "
          "  v = m.get(1) "
          "  hit: bool = m.contains(1) "
          "  m.remove(1) "
          "  return v + m.length "
          "} "
          "x = f()");
}

TEST("TypeChecker accepts add/contains/remove/.length on a Set<i32>")
{
    check("f() -> i32 { "
          "  s = Set<i32>() "
          "  s.add(1) "
          "  hit: bool = s.contains(1) "
          "  s.remove(1) "
          "  return s.length "
          "} "
          "x = f()");
}

TEST("TypeChecker accepts str/bool as Map/Set key types (generic, Rust-style Hash+Eq)")
{
    check("f() { m = Map<str,i32>() }");
    check("f() { m = Map<i32,str>() }");
    check("f() { m = Map<bool,bool>() }");
    check("f() { s = Set<str>() }");
    check("f() { s = Set<bool>() }");
}

TEST("TypeChecker rejects Map/Set as a Map/Set key type (mirrors Rust: HashMap/HashSet aren't "
     "Hash)")
{
    EXPECT_THROWS(check("f() { m = Map<Map<i32,i32>,i32>() }"));
    EXPECT_THROWS(check("f() { s = Set<Set<i32>>() }"));
}

TEST("TypeChecker rejects slice<T> as a Map/Set key or value type")
{
    EXPECT_THROWS(check("f() { m = Map<slice<i32>,i32>() }"));
    EXPECT_THROWS(check("f() { m = Map<i32,slice<i32>>() }"));
}

TEST("TypeChecker accepts a struct key if every field is itself hashable")
{
    check("struct Point { x: i32  y: i32 } "
          "f() { s = Set<Point>() }");
}

TEST("TypeChecker rejects a struct key if any field is not hashable (List<T> field)")
{
    // List<T> itself is already rejected as a struct field type
    // (docs/language/0033-lists.md), so this struct never type-checks in
    // the first place - confirms the rejection surfaces here too, not just
    // at the struct declaration.
    EXPECT_THROWS(check("struct Bag { items: List<i32> } "
                        "f() { s = Set<Bag>() }"));
}

TEST("TypeChecker accepts a fixed array or List<T> key if the element is hashable")
{
    check("f() { s = Set<[i32;3]>() }");
    check("f() { s = Set<List<i32>>() }");
}

TEST("TypeChecker accepts arbitrary V (struct, array, List, nested Map) with no hashability "
     "requirement")
{
    check("struct Point { x: i32 } "
          "f() { "
          "  m1 = Map<i32,Point>() "
          "  m2 = Map<i32,[i32;3]>() "
          "  m3 = Map<i32,List<i32>>() "
          "  m4 = Map<i32,Map<i32,i32>>() "
          "}");
}

TEST("TypeChecker's Map<K,V>.get() returns V's real resolved type, not always i32")
{
    check("struct Point { x: i32 } "
          "f() -> i32 { "
          "  m = Map<i32,Point>() "
          "  p = m.get(1) "
          "  return p.x "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects 'set' with the wrong argument count or type")
{
    EXPECT_THROWS(check("f() { m = Map<i32,i32>()  m.set(1) }"));
    EXPECT_THROWS(check("f() { m = Map<i32,i32>()  m.set(true, 1) }"));
    EXPECT_THROWS(check("f() { m = Map<i32,i32>()  m.set(1, true) }"));
}

TEST("TypeChecker rejects an unknown method on Map/Set")
{
    EXPECT_THROWS(check("f() { m = Map<i32,i32>()  m.size() }"));
    EXPECT_THROWS(check("f() { s = Set<i32>()  s.push(1) }"));
}

TEST("TypeChecker rejects indexing into a Map or Set")
{
    EXPECT_THROWS(check("f() -> i32 { m = Map<i32,i32>()  return m[0] }"));
    EXPECT_THROWS(check("f() -> i32 { s = Set<i32>()  return s[0] }"));
}

TEST("TypeChecker accepts Map<i32,i32>/Set<i32> as a parameter, return type, and local "
     "declared type")
{
    check("build() -> Map<i32,i32> { "
          "  x: Map<i32,i32> = Map<i32,i32>() "
          "  return x "
          "} "
          "use(m: Map<i32,i32>) -> i32 { return m.length } "
          "n = build() "
          "y = use(n)");
}

TEST("TypeChecker rejects Map<K,V>/Set<T> as a struct field type")
{
    EXPECT_THROWS(check("struct Wrapper { entries: Map<i32,i32> }"));
    EXPECT_THROWS(check("struct Wrapper { items: Set<i32> }"));
}
