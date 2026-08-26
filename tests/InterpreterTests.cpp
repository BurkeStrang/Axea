#include "TestFramework.hpp"

#include "interpreter/Interpreter.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"

#include <sstream>

namespace
{
    Program parse(const std::string& source)
    {
        Lexer lexer(source);
        Parser parser(lexer.lex());
        return parser.parseProgram();
    }

    std::unordered_map<std::string, Value> runProgram(const std::string& source)
    {
        auto program = parse(source);
        Interpreter interpreter;
        interpreter.run(program);
        return interpreter.variables();
    }

    Value run(const std::string& source)
    {
        return runProgram(source).at("x");
    }
} // namespace

TEST("Interpreter evaluates arithmetic with operator precedence")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = 1 + 2 * 3")), 7);
}

TEST("Interpreter evaluates parenthesized expressions")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = (1 + 2) * 3 - 1")), 8);
}

TEST("Interpreter throws on division by zero")
{
    EXPECT_THROWS(run("x = 1 / 0"));
}

TEST("Interpreter throws on undefined variable")
{
    EXPECT_THROWS(run("y = x + 1"));
}

TEST("Interpreter evaluates boolean literals")
{
    EXPECT_EQ(std::get<bool>(run("x = true")), true);
    EXPECT_EQ(std::get<bool>(run("x = false")), false);
}

TEST("Interpreter evaluates string literals")
{
    EXPECT_EQ(std::get<std::string>(run(R"(x = "hello")")), "hello");
}

TEST("Interpreter evaluates comparison operators")
{
    EXPECT_EQ(std::get<bool>(run("x = 1 < 2")), true);
    EXPECT_EQ(std::get<bool>(run("x = 2 <= 2")), true);
    EXPECT_EQ(std::get<bool>(run("x = 3 > 2")), true);
    EXPECT_EQ(std::get<bool>(run("x = 2 >= 3")), false);
    EXPECT_EQ(std::get<bool>(run("x = 2 == 2")), true);
    EXPECT_EQ(std::get<bool>(run("x = 2 != 2")), false);
}

TEST("Interpreter evaluates comparisons with correct precedence against arithmetic")
{
    EXPECT_EQ(std::get<bool>(run("x = 1 + 1 == 2")), true);
}

TEST("Interpreter evaluates if-expression true and false branches")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = if 1 < 2 { 10 } else { 20 }")), 10);
    EXPECT_EQ(std::get<std::int64_t>(run("x = if 1 > 2 { 10 } else { 20 }")), 20);
}

TEST("Interpreter throws when if condition is not a boolean")
{
    EXPECT_THROWS(run("x = if 1 { 10 } else { 20 }"));
}

TEST("Interpreter throws when arithmetic operand is not an integer")
{
    EXPECT_THROWS(run(R"(x = 1 + "oops")"));
}

TEST("Interpreter calls a function and evaluates its block result")
{
    EXPECT_EQ(std::get<std::int64_t>(run("square(n: i32) -> i32 { return n * n }  x = square(6)")),
              36);
}

TEST("Interpreter evaluates a fat-arrow function body")
{
    EXPECT_EQ(std::get<std::int64_t>(run("double(n: i32) -> i32 => n * 2  x = double(21)")), 42);
}

TEST("Interpreter handles recursive calls via early return and forward reference")
{
    const std::string source = "factorial(n: i32) -> i32 { "
                               "  if n <= 1 { return 1 } "
                               "  return n * factorial(n - 1) "
                               "} "
                               "x = factorial(5)";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 120);
}

TEST("Interpreter functions do not see top-level globals")
{
    EXPECT_THROWS(runProgram("g = 100  f() -> i32 { return g }  x = f()"));
}

TEST("Interpreter function parameters shadow same-named top-level variables independently")
{
    // The top-level `n` and the parameter `n` never interact.
    const std::string source = "n = 999  identity(n: i32) -> i32 { return n }  x = identity(7)";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 7);
}

TEST("Interpreter throws on wrong argument count")
{
    EXPECT_THROWS(runProgram("f(a: i32, b: i32) -> i32 { return a + b }  x = f(1)"));
}

TEST("Interpreter throws on call to an undefined function")
{
    EXPECT_THROWS(runProgram("x = missing(1)"));
}

TEST("Interpreter constructs a struct and reads its fields")
{
    const std::string source = "struct Point { x: i32  y: i32 } "
                               "p = Point { x: 3  y: 4 } "
                               "x = p.x + p.y";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 7);
}

TEST("Interpreter builds struct literal fields in declared order regardless of source order")
{
    const std::string source = "struct Point { x: i32  y: i32 } "
                               "x = Point { y: 2  x: 1 }";
    const auto instance = std::get<std::shared_ptr<StructInstance>>(run(source));
    EXPECT_EQ(instance->fields[0].first, "x");
    EXPECT_EQ(instance->fields[1].first, "y");
}

TEST("Interpreter throws on struct literal missing a field")
{
    EXPECT_THROWS(runProgram("struct Point { x: i32  y: i32 }  x = Point { x: 1 }"));
}

TEST("Interpreter throws on field access on a non-struct value")
{
    EXPECT_THROWS(run("x = (1).field"));
}

TEST("Interpreter throws on access to an undefined field")
{
    EXPECT_THROWS(runProgram("struct Point { x: i32 }  p = Point { x: 1 }  x = p.y"));
}

TEST("toString formats a struct instance deterministically by declared field order")
{
    const std::string source = "struct Point { x: i32  y: i32 } "
                               "x = Point { x: 1  y: 2 }";
    EXPECT_EQ(toString(run(source)), "Point { x: 1, y: 2 }");
}

TEST("Interpreter field assignment mutates the shared struct instance")
{
    const std::string source = "struct Point { x: i32 } "
                               "update(p: Point) -> i32 { p.x = 99  return p.x } "
                               "p = Point { x: 1 } "
                               "called = update(p) "
                               "x = p.x";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 99);
}

TEST("Interpreter field increment mutates the shared struct instance")
{
    const std::string source = "struct Point { x: i32 } "
                               "bump(p: Point) -> i32 { p.x++  return p.x } "
                               "p = Point { x: 1 } "
                               "called = bump(p) "
                               "x = p.x";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 2);
}

TEST("Interpreter increment of a plain parameter mutates it through nested blocks")
{
    const std::string source = "bump(n: i32) -> i32 { "
                               "  if n > 0 { n++ } "
                               "  return n "
                               "} "
                               "x = bump(5)";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 6);
}

TEST("Interpreter decrement works on a plain parameter")
{
    EXPECT_EQ(std::get<std::int64_t>(run("f(n: i32) -> i32 { n--  return n }  x = f(5)")), 4);
}

TEST("Interpreter runs a while loop, mutating an outer variable via plain assignment")
{
    const std::string source = "sumTo(limit: i32) -> i32 { "
                               "  n = 0  total = 0 "
                               "  while n < limit { n = n + 1  total = total + n } "
                               "  return total "
                               "} "
                               "x = sumTo(5)";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 15);
}

TEST("Interpreter runs an infinite loop that exits via break with a value")
{
    const std::string source = "findFirstOver(limit: i32) -> i32 { "
                               "  n = 0 "
                               "  return loop { "
                               "    n = n + 1 "
                               "    if n > limit { break n } "
                               "  } "
                               "} "
                               "x = findFirstOver(8)";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 9);
}

TEST("Interpreter continue skips the rest of the current iteration")
{
    // Sums only odd numbers from 1 to limit.
    const std::string source = "sumOdds(limit: i32) -> i32 { "
                               "  n = 0  total = 0 "
                               "  while n < limit { "
                               "    n = n + 1 "
                               "    if n / 2 * 2 == n { continue } "
                               "    total = total + n "
                               "  } "
                               "  return total "
                               "} "
                               "x = sumOdds(6)"; // 1 + 3 + 5 = 9
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 9);
}

TEST("Interpreter nested loops: an inner break does not affect the outer loop")
{
    const std::string source = "f() -> i32 { "
                               "  total = 0 "
                               "  i = 0 "
                               "  while i < 3 { "
                               "    i = i + 1 "
                               "    j = 0 "
                               "    while true { "
                               "      j = j + 1 "
                               "      if j > 2 { break } "
                               "      total = total + 1 "
                               "    } "
                               "  } "
                               "  return total "
                               "} "
                               "x = f()"; // 3 outer iterations * 2 inner increments = 6
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 6);
}

TEST("Interpreter bare break exits a while loop early, discarding no useful value")
{
    const std::string source = "f() -> i32 { "
                               "  n = 0 "
                               "  while true { "
                               "    n = n + 1 "
                               "    if n == 4 { break } "
                               "  } "
                               "  return n "
                               "} "
                               "x = f()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 4);
}

TEST("Interpreter for-in sums a range with an exclusive upper bound")
{
    const std::string source = "f() -> i32 { "
                               "  total = 0 "
                               "  for i in 0..5 { total = total + i } "
                               "  return total "
                               "} "
                               "x = f()"; // 0+1+2+3+4 = 10
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 10);
}

TEST("Interpreter break inside a for-in loop exits early")
{
    const std::string source = "f() -> i32 { "
                               "  count = 0 "
                               "  for i in 0..10 { "
                               "    if i == 4 { break } "
                               "    count = count + 1 "
                               "  } "
                               "  return count "
                               "} "
                               "x = f()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 4);
}

TEST("Interpreter continue inside a for-in loop skips the rest of that iteration")
{
    const std::string source = "f() -> i32 { "
                               "  total = 0 "
                               "  for i in 0..6 { "
                               "    if i / 2 * 2 == i { continue } "
                               "    total = total + i "
                               "  } "
                               "  return total "
                               "} "
                               "x = f()"; // 1 + 3 + 5 = 9
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 9);
}

TEST("Interpreter for-in's induction variable never leaks or mutates a same-named outer variable")
{
    const std::string source = "f() -> i32 { "
                               "  i = 99 "
                               "  for i in 0..3 { } "
                               "  return i "
                               "} "
                               "x = f()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 99);
}

TEST("Interpreter nested for-in loops reusing the same variable name do not collide")
{
    const std::string source = "f() -> i32 { "
                               "  total = 0 "
                               "  for i in 0..3 { for i in 0..2 { total = total + 1 } } "
                               "  return total "
                               "} "
                               "x = f()"; // 3 * 2 = 6
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 6);
}

TEST("Interpreter builds an array literal and indexes into it")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = [10, 20, 30][1]")), 20);
}

TEST("Interpreter's .length reports an array literal's element count")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = [10, 20, 30].length")), 3);
}

TEST("Interpreter index-assignment mutates the shared array instance")
{
    const std::string source = "bump(values: [i32; 3]) -> i32 { values[1] = 99  return values[1] } "
                               "values = [1, 2, 3] "
                               "called = bump(values) "
                               "x = values[1]";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 99);
}

TEST("Interpreter throws on a runtime out-of-range index")
{
    EXPECT_THROWS(
        runProgram("f(i: i32) -> i32 { values = [1, 2, 3]  return values[i] }  x = f(5)"));
}

TEST("Interpreter throws on index-assignment out of range")
{
    EXPECT_THROWS(runProgram("f(i: i32) { values = [1, 2, 3]  values[i] = 9 }  y = f(5)  x = 1"));
}

TEST("Interpreter for-in-over-an-array sums its elements")
{
    const std::string source = "sum(values: [i32; 4]) -> i32 { "
                               "  total = 0 "
                               "  for v in values { total = total + v } "
                               "  return total "
                               "} "
                               "x = sum([1, 2, 3, 4])"; // 10
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 10);
}

TEST("Interpreter break inside a for-in-over-an-array loop exits early")
{
    const std::string source = "f(values: [i32; 5]) -> i32 { "
                               "  count = 0 "
                               "  for v in values { "
                               "    if v == 3 { break } "
                               "    count = count + 1 "
                               "  } "
                               "  return count "
                               "} "
                               "x = f([1, 2, 3, 4, 5])";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 2);
}

TEST("Interpreter toString formats an array literally, matching array-literal syntax")
{
    EXPECT_EQ(toString(run("x = [1, 2, 3]")), "[1, 2, 3]");
}

TEST("Interpreter accepts arrays of different sizes through the same slice<T> parameter")
{
    const std::string source = "sum(values: slice<i32>) -> i32 { "
                               "  total = 0 "
                               "  for v in values { total = total + v } "
                               "  return total "
                               "} "
                               "f() -> i32 { return sum([1, 2, 3]) + sum([1, 2, 3, 4, 5]) } "
                               "x = f()"; // 6 + 15 = 21
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 21);
}

TEST("Interpreter's .length on a slice reports the actual passed-in array's size, not a fixed one")
{
    const std::string source = "len(values: slice<i32>) -> i32 { return values.length } "
                               "f() -> i32 { return len([1, 2]) + len([1, 2, 3, 4, 5, 6, 7]) } "
                               "x = f()"; // 2 + 7 = 9
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 9);
}

TEST("Interpreter index-assignment through a slice parameter writes through to the caller's array")
{
    const std::string source = "zeroFirst(values: slice<i32>) { values[0] = 0 } "
                               "a = [1, 2, 3] "
                               "called = zeroFirst(a) "
                               "x = a[0]";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 0);
}

TEST("Interpreter forwards an existing slice to another slice parameter without double-wrapping")
{
    const std::string source =
        "helper(values: slice<i32>) -> i32 { return values[0] + values.length } "
        "wrapper(values: slice<i32>) -> i32 { return helper(values) } "
        "x = wrapper([7, 8, 9])"; // 7 + 3 = 10
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 10);
}

TEST("Interpreter pushes, indexes, and reads .length on a List<T>")
{
    const std::string source = "f() -> i32 { "
                               "  numbers = List<i32>() "
                               "  numbers.push(10) "
                               "  numbers.push(20) "
                               "  numbers.push(30) "
                               "  return numbers[0] + numbers[2] + numbers.length "
                               "} "
                               "x = f()"; // 10 + 30 + 3 = 43
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 43);
}

TEST("Interpreter pop removes and returns the last element, shrinking .length")
{
    const std::string source = "f() -> i32 { "
                               "  numbers = List<i32>() "
                               "  numbers.push(1) "
                               "  numbers.push(2) "
                               "  numbers.push(3) "
                               "  last = numbers.pop() "
                               "  return last * 100 + numbers.length "
                               "} "
                               "x = f()"; // 300 + 2 = 302
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 302);
}

TEST("Interpreter throws on pop from an empty List")
{
    EXPECT_THROWS(runProgram("x = List<i32>().pop()"));
}

TEST("Interpreter index-assignment on a List mutates it in place")
{
    const std::string source = "f() -> i32 { "
                               "  numbers = List<i32>() "
                               "  numbers.push(1) "
                               "  numbers[0] = 99 "
                               "  return numbers[0] "
                               "} "
                               "x = f()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 99);
}

TEST("Interpreter for-in-over-a-List sums its elements")
{
    const std::string source = "f() -> i32 { "
                               "  numbers = List<i32>() "
                               "  numbers.push(1) "
                               "  numbers.push(2) "
                               "  numbers.push(3) "
                               "  total = 0 "
                               "  for v in numbers { total = total + v } "
                               "  return total "
                               "} "
                               "x = f()"; // 6
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 6);
}

TEST("Interpreter push through a List<T> parameter writes through to the caller")
{
    const std::string source = "appendOne(numbers: List<i32>) { numbers.push(99) } "
                               "a = List<i32>() "
                               "called = appendOne(a) "
                               "x = a[0]";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 99);
}

TEST("Interpreter toString formats a List like an array")
{
    EXPECT_EQ(toString(run("x = List<i32>()")), "[]");
}

TEST("Interpreter pushes, peeks, and pops on a Stack<T>, reading .length")
{
    const std::string source = "f() -> i32 { "
                               "  s = Stack<i32>() "
                               "  s.push(10) "
                               "  s.push(20) "
                               "  s.push(30) "
                               "  top = s.peek() "
                               "  last = s.pop() "
                               "  return top * 100 + last * 10 + s.length "
                               "} "
                               "x = f()"; // 3000 + 300 + 2 = 3302
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 3302);
}

TEST("Interpreter peek does not remove, unlike pop")
{
    const std::string source = "f() -> i32 { "
                               "  s = Stack<i32>() "
                               "  s.push(1) "
                               "  s.push(2) "
                               "  a = s.peek() "
                               "  b = s.peek() "
                               "  return a + b + s.length "
                               "} "
                               "x = f()"; // 2 + 2 + 2 = 6, peek is idempotent
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 6);
}

TEST("Interpreter throws on pop from an empty Stack")
{
    EXPECT_THROWS(runProgram("x = Stack<i32>().pop()"));
}

TEST("Interpreter throws on peek of an empty Stack")
{
    EXPECT_THROWS(runProgram("x = Stack<i32>().peek()"));
}

TEST("Interpreter push through a Stack<T> parameter writes through to the caller")
{
    const std::string source = "pushOne(s: Stack<i32>) { s.push(99) } "
                               "a = Stack<i32>() "
                               "called = pushOne(a) "
                               "x = a.peek()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 99);
}

TEST("Interpreter toString formats a Stack like an array (order-preserving, unlike Map/Set)")
{
    EXPECT_EQ(toString(run("x = Stack<i32>()")), "[]");
}

TEST("Interpreter List<T> and Stack<T> push/pop resolve independently on the same-shaped element "
     "type")
{
    const std::string source = "f() -> i32 { "
                               "  l = List<i32>() "
                               "  l.push(1) "
                               "  s = Stack<i32>() "
                               "  s.push(2) "
                               "  return l.pop() * 10 + s.pop() "
                               "} "
                               "x = f()"; // 10 + 2 = 12
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 12);
}

TEST("Interpreter push_front/push_back/pop_front/pop_back on a LinkedList<T>, reading .length")
{
    const std::string source = "f() -> i32 { "
                               "  s = LinkedList<i32>() "
                               "  s.push_back(10) "
                               "  s.push_back(20) "
                               "  s.push_front(5) "
                               "  front = s.pop_front() "
                               "  back = s.pop_back() "
                               "  return front * 100 + back * 10 + s.length "
                               "} "
                               "x = f()"; // 500 + 200 + 1 = 701
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 701);
}

TEST("Interpreter throws on pop_front from an empty LinkedList")
{
    EXPECT_THROWS(runProgram("x = LinkedList<i32>().pop_front()"));
}

TEST("Interpreter throws on pop_back from an empty LinkedList")
{
    EXPECT_THROWS(runProgram("x = LinkedList<i32>().pop_back()"));
}

TEST("Interpreter push_front through a LinkedList<T> parameter writes through to the caller")
{
    const std::string source = "pushOne(s: LinkedList<i32>) { s.push_front(99) } "
                               "a = LinkedList<i32>() "
                               "called = pushOne(a) "
                               "x = a.pop_front()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 99);
}

TEST("Interpreter toString formats a LinkedList as a count only, unlike List/Stack's bracket "
     "format")
{
    const std::string source = "x = LinkedList<i32>() "
                               "b = x.push_back(1) "
                               "c = x.push_back(2)";
    EXPECT_EQ(toString(run(source)), "LinkedList(2 entries)");
}

TEST("Interpreter push_front/push_back/pop_front/pop_back on a Deque<T>, reading .length")
{
    const std::string source = "f() -> i32 { "
                               "  d = Deque<i32>() "
                               "  d.push_back(10) "
                               "  d.push_back(20) "
                               "  d.push_front(5) "
                               "  front = d.pop_front() "
                               "  back = d.pop_back() "
                               "  return front * 100 + back * 10 + d.length "
                               "} "
                               "x = f()"; // 500 + 200 + 1 = 701
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 701);
}

TEST("Interpreter indexes into a Deque<T> with [i], and supports [i] = assignment")
{
    const std::string source = "f() -> i32 { "
                               "  d = Deque<i32>() "
                               "  d.push_back(10) "
                               "  d.push_back(20) "
                               "  d.push_front(5) "
                               "  d[1] = 99 "
                               "  return d[0] + d[1] + d[2] "
                               "} "
                               "x = f()"; // 5 + 99 + 20 = 124
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 124);
}

TEST("Interpreter for-in iterates a Deque<T> - the first collection this session where for-in "
     "works with no dedicated Parser/IrGenerator support, purely via the shared asIndexable "
     "mechanism (see docs/language/0037-deques.md)")
{
    const std::string source = "f() -> i32 { "
                               "  d = Deque<i32>() "
                               "  d.push_back(1) "
                               "  d.push_back(2) "
                               "  d.push_back(3) "
                               "  total = 0 "
                               "  for v in d { total = total + v } "
                               "  return total "
                               "} "
                               "x = f()"; // 1 + 2 + 3 = 6
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 6);
}

TEST("Interpreter throws on pop_front from an empty Deque")
{
    EXPECT_THROWS(runProgram("x = Deque<i32>().pop_front()"));
}

TEST("Interpreter throws on pop_back from an empty Deque")
{
    EXPECT_THROWS(runProgram("x = Deque<i32>().pop_back()"));
}

TEST("Interpreter push_front through a Deque<T> parameter writes through to the caller")
{
    const std::string source = "pushOne(d: Deque<i32>) { d.push_front(99) } "
                               "a = Deque<i32>() "
                               "called = pushOne(a) "
                               "x = a.pop_front()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 99);
}

TEST("Interpreter toString formats a Deque with full bracket contents, unlike LinkedList/Map/"
     "Set's count-only fallback")
{
    const std::string source = "x = Deque<i32>() "
                               "b = x.push_back(1) "
                               "c = x.push_front(0)";
    EXPECT_EQ(toString(run(source)), "[0, 1]");
}

TEST("Interpreter LinkedList<T> and Deque<T> push_front/pop_front resolve independently on the "
     "same-shaped element type")
{
    const std::string source = "f() -> i32 { "
                               "  l = LinkedList<i32>() "
                               "  l.push_front(1) "
                               "  d = Deque<i32>() "
                               "  d.push_front(2) "
                               "  return l.pop_front() * 10 + d.pop_front() "
                               "} "
                               "x = f()"; // 10 + 2 = 12
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 12);
}

TEST("Interpreter enqueue/dequeue on a Queue<T>, reading .length (classic FIFO order)")
{
    const std::string source = "f() -> i32 { "
                               "  q = Queue<i32>() "
                               "  q.enqueue(10) "
                               "  q.enqueue(20) "
                               "  q.enqueue(30) "
                               "  first = q.dequeue() "
                               "  return first * 100 + q.length "
                               "} "
                               "x = f()"; // 1000 + 2 = 1002
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 1002);
}

TEST("Interpreter throws on dequeue from an empty Queue")
{
    EXPECT_THROWS(runProgram("x = Queue<i32>().dequeue()"));
}

TEST("Interpreter enqueue through a Queue<T> parameter writes through to the caller")
{
    const std::string source = "pushOne(q: Queue<i32>) { q.enqueue(99) } "
                               "a = Queue<i32>() "
                               "called = pushOne(a) "
                               "x = a.dequeue()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 99);
}

TEST("Interpreter toString formats a Queue with full bracket contents, unlike LinkedList/Map/"
     "Set's count-only fallback")
{
    const std::string source = "x = Queue<i32>() "
                               "b = x.enqueue(1) "
                               "c = x.enqueue(2)";
    EXPECT_EQ(toString(run(source)), "[1, 2]");
}

TEST("Interpreter Deque<T> and Queue<T> are independent types despite the same underlying "
     "header shape (see docs/language/0038-queues.md)")
{
    const std::string source = "g() -> i32 { "
                               "  d = Deque<i32>() "
                               "  d.push_back(1) "
                               "  q = Queue<i32>() "
                               "  q.enqueue(2) "
                               "  return d.pop_back() * 10 + q.dequeue() "
                               "} "
                               "x = g()"; // 10 + 2 = 12
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 12);
}

TEST("Interpreter pushes, peeks, and pops on a PriorityQueue<T>, always returning the minimum, "
     "reading .length")
{
    const std::string source = "f() -> i32 { "
                               "  q = PriorityQueue<i32>() "
                               "  q.push(30) "
                               "  q.push(10) "
                               "  q.push(20) "
                               "  top = q.peek() "
                               "  smallest = q.pop() "
                               "  return top * 100 + smallest * 10 + q.length "
                               "} "
                               "x = f()"; // 1000 + 100 + 2 = 1102
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 1102);
}

TEST("Interpreter PriorityQueue peek does not remove, unlike pop")
{
    const std::string source = "f() -> i32 { "
                               "  q = PriorityQueue<i32>() "
                               "  q.push(5) "
                               "  q.push(2) "
                               "  a = q.peek() "
                               "  b = q.peek() "
                               "  return a + b + q.length "
                               "} "
                               "x = f()"; // 2 + 2 + 2 = 6, peek is idempotent
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 6);
}

TEST("Interpreter PriorityQueue pop always drains in ascending order regardless of push order")
{
    const std::string source = "drain(q: PriorityQueue<i32>) -> i32 { "
                               "  total = 0 "
                               "  while q.length > 0 { "
                               "    total = total * 1000 + q.pop() "
                               "  } "
                               "  return total "
                               "} "
                               "q = PriorityQueue<i32>() "
                               "a = q.push(50) "
                               "b = q.push(10) "
                               "c = q.push(30) "
                               "x = drain(q)"; // digits 10, 30, 50 in order -> 10030050
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 10030050);
}

TEST("Interpreter PriorityQueue<char> pop drains in ascending codepoint order, mirroring "
     "PriorityQueue<i32>'s own ordering (see docs/language/0044-char.md)")
{
    auto vars = runProgram("q = PriorityQueue<char>() "
                           "a = q.push('C') b = q.push('A') c = q.push('B') "
                           "x = q.pop() y = q.pop() z = q.pop()");
    EXPECT_EQ(toString(vars.at("x")), "A");
    EXPECT_EQ(toString(vars.at("y")), "B");
    EXPECT_EQ(toString(vars.at("z")), "C");
}

TEST("Interpreter PriorityQueue<str> pop drains in ascending lexicographic order, mirroring "
     "PriorityQueue<i32>/PriorityQueue<char>'s own ordering (see docs/language/0042-string.md)")
{
    auto vars = runProgram("q = PriorityQueue<str>() "
                           "a = q.push(\"cherry\") b = q.push(\"apple\") c = q.push(\"banana\") "
                           "x = q.pop() y = q.pop() z = q.pop()");
    EXPECT_EQ(toString(vars.at("x")), "apple");
    EXPECT_EQ(toString(vars.at("y")), "banana");
    EXPECT_EQ(toString(vars.at("z")), "cherry");
}

TEST("Interpreter throws on pop from an empty PriorityQueue")
{
    EXPECT_THROWS(runProgram("x = PriorityQueue<i32>().pop()"));
}

TEST("Interpreter throws on peek of an empty PriorityQueue")
{
    EXPECT_THROWS(runProgram("x = PriorityQueue<i32>().peek()"));
}

TEST("Interpreter push through a PriorityQueue<T> parameter writes through to the caller")
{
    const std::string source = "pushOne(q: PriorityQueue<i32>) { q.push(99) } "
                               "a = PriorityQueue<i32>() "
                               "called = pushOne(a) "
                               "x = a.peek()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 99);
}

TEST("Interpreter toString formats a PriorityQueue with bracket contents in heap order, not "
     "sorted order")
{
    const std::string source = "x = PriorityQueue<i32>() "
                               "a = x.push(30) "
                               "b = x.push(10) "
                               "c = x.push(20)";
    EXPECT_EQ(toString(run(source)), "[10, 30, 20]");
}

TEST("Interpreter List<T>/Stack<T>/PriorityQueue<T> push/pop resolve independently on the "
     "same-shaped element type")
{
    const std::string source = "f() -> i32 { "
                               "  l = List<i32>() "
                               "  l.push(1) "
                               "  s = Stack<i32>() "
                               "  s.push(2) "
                               "  q = PriorityQueue<i32>() "
                               "  q.push(3) "
                               "  return l.pop() * 100 + s.pop() * 10 + q.pop() "
                               "} "
                               "x = f()"; // 100 + 20 + 3 = 123
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 123);
}

TEST("Interpreter set/get/contains/remove round-trip on a Map<i32,i32>")
{
    const std::string source = "f() -> i32 { "
                               "  m = Map<i32,i32>() "
                               "  m.set(1, 100) "
                               "  m.set(2, 200) "
                               "  m.set(1, 999) " // update, not a duplicate
                               "  before = m.contains(2) "
                               "  m.remove(2) "
                               "  after = m.contains(2) "
                               "  removedDelta = if before { 10 } else { 0 } "
                               "  keptDelta = if after { 1 } else { 0 } "
                               "  return m.get(1) + m.length * 1000 + removedDelta + keptDelta "
                               "} "
                               "x = f()"; // 999 + 1000 + 10 + 0 = 2009
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 2009);
}

TEST("Interpreter Map.get throws on a missing key")
{
    EXPECT_THROWS(runProgram("x = Map<i32,i32>().get(1)"));
}

TEST("Interpreter add/contains/remove round-trip on a Set<i32>")
{
    const std::string source = "f() -> i32 { "
                               "  s = Set<i32>() "
                               "  s.add(5) "
                               "  s.add(6) "
                               "  s.add(5) " // duplicate add is a no-op
                               "  before = s.contains(6) "
                               "  s.remove(6) "
                               "  after = s.contains(6) "
                               "  removedDelta = if before { 10 } else { 0 } "
                               "  keptDelta = if after { 1 } else { 0 } "
                               "  return s.length * 1000 + removedDelta + keptDelta "
                               "} "
                               "x = f()"; // 1000 + 10 + 0 = 1010
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 1010);
}

TEST("Interpreter 'set' through a Map<i32,i32> parameter writes through to the caller")
{
    const std::string source = "put(m: Map<i32,i32>) { m.set(1, 42) } "
                               "a = Map<i32,i32>() "
                               "called = put(a) "
                               "x = a.get(1)";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 42);
}

TEST("Interpreter 'add' through a Set<i32> parameter writes through to the caller")
{
    const std::string source = "addOne(s: Set<i32>) { s.add(7) } "
                               "a = Set<i32>() "
                               "called = addOne(a) "
                               "x = a.length";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 1);
}

TEST("Interpreter toString formats Map/Set by count, not contents")
{
    EXPECT_EQ(toString(run("x = Map<i32,i32>()")), "Map(0 entries)");
    EXPECT_EQ(toString(run("x = Set<i32>()")), "Set(0 entries)");
}

TEST("Interpreter Map<str,i32> hashes/compares str keys by content, not identity")
{
    const std::string source = "f() -> i32 { "
                               "  m = Map<str,i32>() "
                               "  m.set(\"a\", 1) "
                               "  m.set(\"a\", 999) " // separately-constructed but equal key
                               "  m.set(\"b\", 2) "
                               "  return m.get(\"a\") * 1000 + m.length "
                               "} "
                               "x = f()"; // 999000 + 2
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 999002);
}

TEST("Interpreter Set<Point> hashes/compares struct keys structurally, not by identity")
{
    const std::string source = "struct Point { x: i32  y: i32 } "
                               "f() -> i32 { "
                               "  s = Set<Point>() "
                               "  s.add(Point { x: 1  y: 2 }) "
                               "  s.add(Point { x: 1  y: 2 }) " // separately-constructed, equal
                               "  s.add(Point { x: 3  y: 4 }) "
                               "  hasSame = s.contains(Point { x: 1  y: 2 }) "
                               "  delta = if hasSame { 100 } else { 0 } "
                               "  return s.length * 1000 + delta "
                               "} "
                               "x = f()"; // 2000 + 100
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 2100);
}

TEST("Interpreter Map<i32,Point>.get() returns an alias to the map's own stored struct")
{
    const std::string source = "struct Point { x: i32 } "
                               "f() -> i32 { "
                               "  m = Map<i32,Point>() "
                               "  m.set(1, Point { x: 1 }) "
                               "  p = m.get(1) "
                               "  p.x = 99 "
                               "  return m.get(1).x "
                               "} "
                               "x = f()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 99);
}

TEST("Interpreter Map<K,V> supports arbitrary V: struct, array, List, nested Map")
{
    const std::string source =
        "struct Point { x: i32 } "
        "f() -> i32 { "
        "  m1 = Map<i32,Point>()  m1.set(1, Point { x: 10 }) "
        "  m2 = Map<i32,[i32;2]>()  m2.set(1, [1, 2]) "
        "  m3 = Map<i32,List<i32>>() "
        "  inner = List<i32>()  inner.push(7) "
        "  m3.set(1, inner) "
        "  m4 = Map<i32,Map<i32,i32>>() "
        "  innerMap = Map<i32,i32>()  innerMap.set(5, 50) "
        "  m4.set(1, innerMap) "
        "  return m1.get(1).x + m2.get(1)[0] + m3.get(1)[0] + m4.get(1).get(5) "
        "} "
        "x = f()"; // 10 + 1 + 7 + 50
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 68);
}

TEST("Interpreter set/get/contains/remove round-trip on a SortedMap<i32,i32>")
{
    const std::string source = "f() -> i32 { "
                               "  m = SortedMap<i32,i32>() "
                               "  m.set(1, 100) "
                               "  m.set(2, 200) "
                               "  m.set(1, 999) " // update, not a duplicate
                               "  before = m.contains(2) "
                               "  m.remove(2) "
                               "  after = m.contains(2) "
                               "  removedDelta = if before { 10 } else { 0 } "
                               "  keptDelta = if after { 1 } else { 0 } "
                               "  return m.get(1) + m.length * 1000 + removedDelta + keptDelta "
                               "} "
                               "x = f()"; // 999 + 1000 + 10 + 0 = 2009
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 2009);
}

TEST("Interpreter SortedMap.get throws on a missing key")
{
    EXPECT_THROWS(runProgram("x = SortedMap<i32,i32>().get(1)"));
}

TEST("Interpreter 'set' through a SortedMap<i32,i32> parameter writes through to the caller")
{
    const std::string source = "put(m: SortedMap<i32,i32>) { m.set(1, 42) } "
                               "a = SortedMap<i32,i32>() "
                               "called = put(a) "
                               "x = a.get(1)";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 42);
}

TEST("Interpreter toString formats a SortedMap by count, not contents - matches the LLVM "
     "backend's own identical choice (see docs/language/0040-sorted-maps.md)")
{
    EXPECT_EQ(toString(run("x = SortedMap<i32,i32>()")), "SortedMap(0 entries)");
}

TEST("Interpreter SortedMap<i32,Point>.get() returns an alias to the tree's own stored struct")
{
    const std::string source = "struct Point { x: i32 } "
                               "f() -> i32 { "
                               "  m = SortedMap<i32,Point>() "
                               "  m.set(1, Point { x: 5 }) "
                               "  p = m.get(1) "
                               "  return p.x "
                               "} "
                               "x = f()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 5);
}

TEST("Interpreter SortedMap keeps keys correctly ordered under insertion and removal in "
     "arbitrary order, verified by exhaustive get() after many inserts/removes/updates "
     "(the same scenario hand-verified against compiled -O0/-O1 output during development - "
     "see docs/language/0040-sorted-maps.md)")
{
    const std::string source =
        "f() -> i32 { "
        "  m = SortedMap<i32,i32>() "
        "  m.set(50, 100)  m.set(30, 101)  m.set(70, 102)  m.set(20, 103) "
        "  m.set(40, 104)  m.set(60, 105)  m.set(80, 106)  m.set(10, 107) "
        "  m.set(25, 108)  m.set(35, 109)  m.set(45, 110)  m.set(90, 111) "
        "  m.set(5, 112)   m.set(1, 113)   m.set(2, 114)   m.set(3, 115) "
        "  m.set(4, 116) "
        "  lenAfterInserts = m.length " // 17
        "  m.remove(50) " // two-children removal, triggers successor splice + rebalance
        "  m.remove(1) "
        "  m.remove(2) "
        "  m.remove(999) "              // no-op, key absent
        "  lenAfterRemoves = m.length " // 14
        "  return m.get(3) + m.get(4) + m.get(5) + m.get(30) + m.get(70) + m.get(90) "
        "    + lenAfterInserts * 10000 + lenAfterRemoves * 100000 "
        "} "
        "x = f()"; // 115+116+112+101+102+111 + 170000 + 1400000 = 1570657
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 1570657);
}

TEST("Interpreter set/get/contains/remove round-trip on a SortedMap<char,i32> - char is "
     "orderable by codepoint, same as i32 (see docs/language/0044-char.md)")
{
    const std::string source = "f() -> i32 { "
                               "  m = SortedMap<char,i32>() "
                               "  m.set('B', 2)  m.set('A', 1)  m.set('C', 3) "
                               "  hit = if m.contains('A') { 1 } else { 0 } "
                               "  m.remove('B') "
                               "  return m.get('A') + m.get('C') + hit * 1000 + m.length * 100000 "
                               "} "
                               "x = f()"; // 1 + 3 + 1000 + 200000 = 201004
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 201004);
}

TEST("Interpreter set/get/contains/remove round-trip on a SortedMap<str,i32> - str has a real "
     "lexicographic order, same as i32/char (see docs/language/0042-string.md)")
{
    const std::string source =
        "f() -> i32 { "
        "  m = SortedMap<str,i32>() "
        "  m.set(\"b\", 2)  m.set(\"a\", 1)  m.set(\"c\", 3) "
        "  hit = if m.contains(\"a\") { 1 } else { 0 } "
        "  m.remove(\"b\") "
        "  return m.get(\"a\") + m.get(\"c\") + hit * 1000 + m.length * 100000 "
        "} "
        "x = f()"; // 1 + 3 + 1000 + 200000 = 201004
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 201004);
}

TEST("Interpreter add/contains/remove round-trip on a SortedSet<i32>")
{
    const std::string source = "f() -> i32 { "
                               "  s = SortedSet<i32>() "
                               "  s.add(5) "
                               "  s.add(6) "
                               "  s.add(5) " // duplicate add is a no-op
                               "  before = s.contains(6) "
                               "  s.remove(6) "
                               "  after = s.contains(6) "
                               "  removedDelta = if before { 10 } else { 0 } "
                               "  keptDelta = if after { 1 } else { 0 } "
                               "  return s.length * 1000 + removedDelta + keptDelta "
                               "} "
                               "x = f()"; // 1000 + 10 + 0 = 1010
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 1010);
}

TEST("Interpreter add/contains/remove round-trip on a SortedSet<char> - char is orderable by "
     "codepoint, same as i32 (see docs/language/0044-char.md)")
{
    const std::string source = "f() -> i32 { "
                               "  s = SortedSet<char>() "
                               "  s.add('A')  s.add('B')  s.add('A') " // duplicate add is a no-op
                               "  before = if s.contains('B') { 1 } else { 0 } "
                               "  s.remove('B') "
                               "  after = if s.contains('B') { 1 } else { 0 } "
                               "  return s.length * 1000 + before * 10 + after "
                               "} "
                               "x = f()"; // 1000 + 10 + 0 = 1010
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 1010);
}

TEST("Interpreter add/contains/remove round-trip on a SortedSet<str> - str has a real "
     "lexicographic order, same as i32/char (see docs/language/0042-string.md)")
{
    const std::string source = "f() -> i32 { "
                               "  s = SortedSet<str>() "
                               "  s.add(\"a\")  s.add(\"b\")  s.add(\"a\") " // dup add is a no-op
                               "  before = if s.contains(\"b\") { 1 } else { 0 } "
                               "  s.remove(\"b\") "
                               "  after = if s.contains(\"b\") { 1 } else { 0 } "
                               "  return s.length * 1000 + before * 10 + after "
                               "} "
                               "x = f()"; // 1000 + 10 + 0 = 1010
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 1010);
}

TEST("Interpreter 'add' through a SortedSet<i32> parameter writes through to the caller")
{
    const std::string source = "addOne(s: SortedSet<i32>) { s.add(7) } "
                               "a = SortedSet<i32>() "
                               "called = addOne(a) "
                               "x = a.length";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 1);
}

TEST("Interpreter toString formats a SortedSet by count, not contents - matches the LLVM "
     "backend's own identical choice (see docs/language/0041-sorted-sets.md)")
{
    EXPECT_EQ(toString(run("x = SortedSet<i32>()")), "SortedSet(0 entries)");
}

TEST("Interpreter Set<T>/Map<K,V>/SortedMap<K,V>/SortedSet<T> contains/remove resolve "
     "independently despite sharing the same method names")
{
    const std::string source = "f() -> i32 { "
                               "  st = Set<i32>()  st.add(1) "
                               "  mp = Map<i32,i32>()  mp.set(1, 20) "
                               "  sm = SortedMap<i32,i32>()  sm.set(1, 300) "
                               "  ss = SortedSet<i32>()  ss.add(1) "
                               "  a = if st.contains(1) { 1 } else { 0 } "
                               "  b = mp.get(1) "
                               "  c = sm.get(1) "
                               "  d = if ss.contains(1) { 1 } else { 0 } "
                               "  return a + b + c + d "
                               "} "
                               "x = f()"; // 1 + 20 + 300 + 1 = 322
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 322);
}

TEST("Interpreter SortedSet keeps elements correctly ordered under insertion and removal in "
     "arbitrary order, verified by exhaustive contains() after many adds/removes (mirrors "
     "SortedMap's own exhaustive stress test - see docs/language/0041-sorted-sets.md)")
{
    const std::string source =
        "f() -> i32 { "
        "  s = SortedSet<i32>() "
        "  s.add(50)  s.add(30)  s.add(70)  s.add(20) "
        "  s.add(40)  s.add(60)  s.add(80)  s.add(10) "
        "  s.add(25)  s.add(35)  s.add(45)  s.add(90) "
        "  s.add(5)   s.add(1)   s.add(2)   s.add(3) "
        "  s.add(4) "
        "  lenAfterInserts = s.length " // 17
        "  s.remove(50) " // two-children removal, triggers successor splice + rebalance
        "  s.remove(1) "
        "  s.remove(2) "
        "  s.remove(999) "              // no-op, element absent
        "  lenAfterRemoves = s.length " // 14
        "  c3 = if s.contains(3) { 1 } else { 0 } "
        "  c4 = if s.contains(4) { 1 } else { 0 } "
        "  c5 = if s.contains(5) { 1 } else { 0 } "
        "  c30 = if s.contains(30) { 1 } else { 0 } "
        "  c70 = if s.contains(70) { 1 } else { 0 } "
        "  c90 = if s.contains(90) { 1 } else { 0 } "
        "  c1 = if s.contains(1) { 1 } else { 0 } " // removed
        "  return c3 + c4 + c5 + c30 + c70 + c90 + c1 "
        "    + lenAfterInserts * 10000 + lenAfterRemoves * 100000 "
        "} "
        "x = f()"; // 6 present + 0 removed + 170000 + 1400000 = 1570006
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 1570006);
}

TEST("Interpreter constructs a String from a str literal and reads .length")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = String(\"Axea\").length")), 4);
}

TEST("Interpreter String.append mutates in place and grows .length")
{
    const std::string source = "f() -> i32 { "
                               "  s = String(\"Axea\") "
                               "  s.append(\" Language\") "
                               "  return s.length "
                               "} "
                               "x = f()"; // 4 + 9 = 13
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 13);
}

TEST("Interpreter toString on a String prints its own content, bare, same as a plain str")
{
    EXPECT_EQ(toString(run("x = String(\"Axea\")")), "Axea");
}

TEST("Interpreter String.append accepts another String, not just a str literal")
{
    const std::string source = "f() -> i32 { "
                               "  a = String(\"Axea\") "
                               "  b = String(\" Language\") "
                               "  a.append(b) "
                               "  return a.length "
                               "} "
                               "x = f()"; // 4 + 9 = 13
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 13);
}

TEST("Interpreter 'append' through a String parameter writes through to the caller")
{
    const std::string source = "appendOne(s: String) { s.append(\"!\") } "
                               "a = String(\"hi\") "
                               "called = appendOne(a) "
                               "x = a.length";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 3);
}

TEST("Interpreter passes a String where a str parameter is expected, as a real independent "
     "snapshot - a later .append() on the source String must not retroactively change an "
     "already-passed str (str is an immutable value, not an alias - see "
     "docs/language/0042-string.md)")
{
    const std::string source = "identity(s: str) -> str { return s } "
                               "f() -> i32 { "
                               "  s = String(\"hi\") "
                               "  snapshot = identity(s) "
                               "  s.append(\"!\") "
                               "  return s.length "
                               "} "
                               "x = f()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 3);
}

TEST("Interpreter String(text) copies text's content rather than aliasing it - a later "
     ".append() on the source must not retroactively change an already-constructed String")
{
    const std::string source = "f() -> i32 { "
                               "  a = String(\"hi\") "
                               "  b = String(a) "
                               "  a.append(\"!\") "
                               "  return b.length "
                               "} "
                               "x = f()"; // b snapshot at 2, unaffected by a's later append
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 2);
}

TEST("Interpreter constructs an empty Buffer with length 0")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = Buffer().length")), 0);
}

TEST("Interpreter Buffer.append mutates in place and grows .length")
{
    const std::string source = "f() -> i32 { "
                               "  b = Buffer() "
                               "  b.append(\"Axea\") "
                               "  b.append(\" Language\") "
                               "  return b.length "
                               "} "
                               "x = f()"; // 4 + 9 = 13
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 13);
}

TEST("Interpreter Buffer.write behaves identically to Buffer.append, including interpolated "
     "arguments (see docs/language/0061-buffer-write.md)")
{
    const std::string source = "f() -> String { "
                               "  name = \"Ada\" "
                               "  age = 30 "
                               "  b = Buffer() "
                               "  b.write(\"Name: {name}\\n\") "
                               "  b.write(\"Age: {age}\\n\") "
                               "  return b.finish() "
                               "} "
                               "x = f()";
    EXPECT_EQ(toString(run(source)), "Name: Ada\\nAge: 30\\n");
}

TEST("Interpreter Buffer.append_line appends its text plus a trailing newline")
{
    const std::string source = "f() -> String { "
                               "  b = Buffer() "
                               "  b.append_line(\"hi\") "
                               "  return b.finish() "
                               "} "
                               "x = f()";
    EXPECT_EQ(toString(run(source)), "hi\n");
}

TEST("Interpreter Buffer.clear resets length to 0 without preventing reuse")
{
    const std::string source = "f() -> i32 { "
                               "  b = Buffer() "
                               "  b.append(\"hello\") "
                               "  b.clear() "
                               "  afterClear = b.length "
                               "  b.append(\"re\") "
                               "  return b.length "
                               "} "
                               "x = f()"; // afterClear = 0 (unused here), final length = 2
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 2);
}

TEST("Interpreter Buffer.reserve grows .capacity without changing .length or content")
{
    const std::string source = "f() -> i32 { "
                               "  b = Buffer() "
                               "  b.reserve(64) "
                               "  return b.length "
                               "} "
                               "x = f()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 0);
    const std::string capSource = "f() -> i32 { "
                                  "  b = Buffer() "
                                  "  b.reserve(64) "
                                  "  return b.capacity "
                                  "} "
                                  "x = f()";
    EXPECT_TRUE(std::get<std::int64_t>(run(capSource)) >= 64);
}

TEST("Interpreter Buffer.capacity tracks the compiled backend's own explicit doubling-growth "
     "algorithm exactly (growBufferCapacity/ensureBufferCapacity) - not std::string::capacity(), "
     "which reports an unrelated, implementation-defined SSO/growth threshold (a real bug found "
     "via a byte-for-byte comparison against the compiled backend, not a hypothetical one)")
{
    // Starts at 1; appending "ab" (needed = 0 + 2 + 1 = 3) needs to grow -
    // doubled (1*2=2) is still short of needed (3), so capacity becomes
    // needed itself: 3.
    EXPECT_EQ(std::get<std::int64_t>(run("b = Buffer()  a = b.append(\"ab\")  x = b.capacity")), 3);
    // Appending "cdefgh" next (needed = 2 + 6 + 1 = 9) - doubled (3*2=6)
    // is still short, so capacity becomes needed: 9.
    EXPECT_EQ(std::get<std::int64_t>(run("b = Buffer() "
                                         "a = b.append(\"ab\") "
                                         "c = b.append(\"cdefgh\") "
                                         "x = b.capacity")),
              9);
    // reserve(100) - unlike append, no +1 for a null terminator; needed
    // (100) exceeds doubled (1*2=2), so capacity becomes needed: 100.
    EXPECT_EQ(std::get<std::int64_t>(run("b = Buffer()  r = b.reserve(100)  x = b.capacity")), 100);
    // clear() leaves capacity untouched - only length resets.
    EXPECT_EQ(std::get<std::int64_t>(run("b = Buffer() "
                                         "r = b.reserve(100) "
                                         "c = b.clear() "
                                         "x = b.capacity")),
              100);
    // finish() resets capacity back to 1, the same fresh state a brand
    // new Buffer() starts at.
    EXPECT_EQ(std::get<std::int64_t>(run("b = Buffer() "
                                         "r = b.reserve(100) "
                                         "t = b.finish() "
                                         "x = b.capacity")),
              1);
}

TEST("Interpreter Buffer.finish transfers content into a fresh String and resets the buffer to "
     "a fresh, empty, reusable state - not left dangling")
{
    const std::string source = "f() -> i32 { "
                               "  b = Buffer() "
                               "  b.append(\"finished content\") "
                               "  s = b.finish() "
                               "  afterFinish = b.length "
                               "  b.append(\"reused\") "
                               "  return b.length "
                               "} "
                               "x = f()"; // afterFinish = 0, final length = 6 ("reused")
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 6);
}

TEST("Interpreter toString on a Buffer.finish() result matches the buffer's own content at the "
     "moment of the call")
{
    const std::string source = "f() -> String { "
                               "  b = Buffer() "
                               "  b.append(\"finished content\") "
                               "  return b.finish() "
                               "} "
                               "x = f()";
    EXPECT_EQ(toString(run(source)), "finished content");
}

TEST("Interpreter Buffer.finish() result is independent of later mutation on the original "
     "buffer - a genuine ownership transfer, not an alias")
{
    const std::string source = "f() -> i32 { "
                               "  b = Buffer() "
                               "  b.append(\"hi\") "
                               "  s = b.finish() "
                               "  b.append(\"!!!\") "
                               "  return s.length "
                               "} "
                               "x = f()"; // s snapshot at 2, unaffected by b's later append
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 2);
}

TEST("Interpreter toString on a Buffer prints its own content, bare, same as String")
{
    EXPECT_EQ(toString(run("b = Buffer()  t = b.append(\"Axea\")  x = b")), "Axea");
}

TEST("Interpreter 'append' through a Buffer parameter writes through to the caller")
{
    const std::string source = "appendOne(b: Buffer) { b.append(\"!\") } "
                               "a = Buffer() "
                               "t = a.append(\"hi\") "
                               "called = appendOne(a) "
                               "x = a.length";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 3);
}

TEST("Interpreter distinguishes Buffer.append from String.append at runtime despite the shared "
     "method name")
{
    const std::string source = "f() -> i32 { "
                               "  buf = Buffer() "
                               "  buf.append(\"ab\") "
                               "  s = String(\"cde\") "
                               "  s.append(\"f\") "
                               "  return buf.length + s.length "
                               "} "
                               "x = f()"; // 2 + 4 = 6
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 6);
}

TEST("Interpreter evaluates a char literal to its own codepoint")
{
    EXPECT_EQ(toString(run("x = 'A'")), "A");
}

TEST("Interpreter toString on a char encodes multi-byte UTF-8 content correctly")
{
    EXPECT_EQ(toString(run("x = 'é'")), "é");
    EXPECT_EQ(toString(run("x = '🚀'")), "🚀");
}

TEST("Interpreter compares char values by equality")
{
    EXPECT_EQ(std::get<bool>(run("x = 'A' == 'A'")), true);
    EXPECT_EQ(std::get<bool>(run("x = 'A' == 'B'")), false);
    EXPECT_EQ(std::get<bool>(run("x = 'A' != 'B'")), true);
}

TEST("Interpreter compares char values by codepoint ordering")
{
    EXPECT_EQ(std::get<bool>(run("x = 'A' < 'B'")), true);
    EXPECT_EQ(std::get<bool>(run("x = 'B' < 'A'")), false);
    EXPECT_EQ(std::get<bool>(run("x = 'A' <= 'A'")), true);
    EXPECT_EQ(std::get<bool>(run("x = 'B' > 'A'")), true);
    EXPECT_EQ(std::get<bool>(run("x = 'A' >= 'A'")), true);
}

TEST("Interpreter evaluates i64 arithmetic and comparisons (see "
     "docs/language/0005-type-system.md)")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = 100i64 + 25i64")), 125);
    EXPECT_EQ(std::get<std::int64_t>(run("x = 100i64 - 25i64")), 75);
    EXPECT_EQ(std::get<std::int64_t>(run("x = 100i64 * 2i64")), 200);
    EXPECT_EQ(std::get<std::int64_t>(run("x = 100i64 / 4i64")), 25);
    EXPECT_EQ(std::get<bool>(run("x = 10i64 < 20i64")), true);
    EXPECT_EQ(std::get<bool>(run("x = 10i64 == 10i64")), true);
}

TEST("Interpreter evaluates f64 arithmetic and comparisons with real floating-point semantics, "
     "not truncating integer division")
{
    EXPECT_EQ(std::get<double>(run("x = 1.5 + 2.5")), 4.0);
    EXPECT_EQ(std::get<double>(run("x = 5.0 - 1.5")), 3.5);
    EXPECT_EQ(std::get<double>(run("x = 1.0 / 4.0")), 0.25);
    EXPECT_EQ(std::get<bool>(run("x = 1.5 < 2.5")), true);
}

TEST("Interpreter's 'as' cast converts between i32/i64/f64 with real value semantics - "
     "sign-extend/truncate between i32/i64 (a no-op at this interpreter's own Value level, "
     "since both share the std::int64_t alternative), truncate-toward-zero from f64 to an "
     "integer, and an exact int-to-float conversion")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = 5 as i64")), 5);
    EXPECT_EQ(std::get<std::int64_t>(run("y = 100i64 x = y as i32")), 100);
    EXPECT_EQ(std::get<double>(run("x = 5 as f64")), 5.0);
    EXPECT_EQ(std::get<std::int64_t>(run("x = 9.7 as i32")), 9);
    EXPECT_EQ(std::get<std::int64_t>(run("x = 9.7 as i64")), 9);
}

TEST("Interpreter prints i64/f64 top-level bindings and print() arguments via a real \"%g\" "
     "format for f64, matching LlvmIrEmitter's own @axea.f64.to_str exactly")
{
    EXPECT_EQ(toString(run("x = 100i64")), "100");
    EXPECT_EQ(toString(run("x = 1.5")), "1.5");
    EXPECT_EQ(toString(run("x = 4.0")), "4"); // %g trims a trailing ".0"
}

TEST("Interpreter PriorityQueue<f64>/PriorityQueue<i64> pop in ascending numeric order, "
     "mirroring PriorityQueue<i32>'s own ordering")
{
    auto vars = runProgram("q = PriorityQueue<f64>() "
                           "a = q.push(3.5) b = q.push(1.5) c = q.push(2.5) "
                           "x = q.pop()");
    EXPECT_EQ(std::get<double>(vars.at("x")), 1.5);
}

TEST("Interpreter compares str values by real lexicographic ordering (see "
     "docs/language/0042-string.md)")
{
    EXPECT_EQ(std::get<bool>(run("x = \"apple\" < \"banana\"")), true);
    EXPECT_EQ(std::get<bool>(run("x = \"banana\" < \"apple\"")), false);
    EXPECT_EQ(std::get<bool>(run("x = \"apple\" <= \"apple\"")), true);
    EXPECT_EQ(std::get<bool>(run("x = \"banana\" > \"apple\"")), true);
    EXPECT_EQ(std::get<bool>(run("x = \"apple\" >= \"apple\"")), true);
    // Shorter-is-less-when-a-strict-prefix, matching a textbook strcmp.
    EXPECT_EQ(std::get<bool>(run("x = \"app\" < \"apple\"")), true);
}

TEST("Interpreter compares str/String values by real content, not identity, even for two "
     "genuinely separately constructed values with equal content - fixes a pointer/shared_ptr-"
     "identity bug this content check previously had (see docs/language/0042-string.md)")
{
    // A str slice result is a fresh, independently allocated buffer (see
    // docs/language/0050-collection-join-and-slicing.md) - definitely not
    // the same object as the "hello" literal below.
    EXPECT_EQ(std::get<bool>(run("source = \"xxhelloxx\"  sliced = source[2..7]  "
                                 "x = sliced == \"hello\"")),
              true);
    EXPECT_EQ(std::get<bool>(run("source = \"xxhelloxx\"  sliced = source[2..7]  "
                                 "x = sliced != \"world\"")),
              true);
    // Two separately-constructed String() instances with equal content -
    // previously compared unequal (shared_ptr identity), the same class of
    // bug the str/i8* case above had at the LLVM-backend level.
    EXPECT_EQ(std::get<bool>(run("a = String(\"hello\")  b = String(\"hello\")  x = a == b")),
              true);
    EXPECT_EQ(std::get<bool>(run("a = String(\"hello\")  b = String(\"world\")  x = a != b")),
              true);
}

TEST("Interpreter passes a char through a function parameter and return value unchanged")
{
    const std::string source = "identity(c: char) -> char { return c } "
                               "x = identity('Z')";
    EXPECT_EQ(toString(run(source)), "Z");
}

TEST("Interpreter reads a char struct field, printed via the struct's own toString")
{
    const std::string source = "struct Letter { value: char } "
                               "x = Letter { value: 'Q' }";
    EXPECT_EQ(toString(run(source)), "Letter { value: Q }");
}

TEST("Interpreter constructs a List<char> and prints it with each element's own Unicode "
     "character, matching the compiled backend's own byte-for-byte encoding")
{
    const std::string source = "xs = List<char>() "
                               "t1 = xs.push('a') "
                               "t2 = xs.push('é') "
                               "t3 = xs.push('🚀') "
                               "x = xs";
    EXPECT_EQ(toString(run(source)), "[a, é, 🚀]");
}

TEST("Interpreter slices a str with a bounded, open-start, open-end, and fully-open range")
{
    EXPECT_EQ(toString(run("date = \"2026-08-18\"  x = date[..4]")), "2026");
    EXPECT_EQ(toString(run("date = \"2026-08-18\"  x = date[5..7]")), "08");
    EXPECT_EQ(toString(run("date = \"2026-08-18\"  x = date[8..]")), "18");
    EXPECT_EQ(toString(run("date = \"2026-08-18\"  x = date[..]")), "2026-08-18");
}

TEST("Interpreter indexes a str/String by real Unicode codepoint, not byte offset - a "
     "multi-byte character correctly counts as one index step (see "
     "docs/language/0047-unicode.md)")
{
    EXPECT_EQ(toString(run("s = \"hello\"  x = s[0]")), "h");
    EXPECT_EQ(toString(run("s = \"hello\"  x = s[4]")), "o");
    // "aébc" - 'é' is a 2-byte UTF-8 sequence but a single codepoint, so
    // index 2 ('c') must land right after it, not one byte short.
    EXPECT_EQ(toString(run("s = \"aébc\"  x = s[1]")), "é");
    EXPECT_EQ(toString(run("s = \"aébc\"  x = s[2]")), "b");
    EXPECT_EQ(toString(run("s = String(\"world\")  x = s[0]")), "w");
}

TEST("Interpreter throws on an out-of-range str/String index")
{
    EXPECT_THROWS(run("s = \"hi\"  x = s[10]"));
    EXPECT_THROWS(run("s = \"hi\"  x = s[0 - 1]"));
}

TEST("Interpreter slices a String, str-coerced the same way .append's own argument is")
{
    const std::string source = "s = String(\"Axea Language\") "
                               "x = s[0..4]";
    EXPECT_EQ(toString(run(source)), "Axea");
}

TEST("Interpreter's str slice is a real, independent copy - mutating the source String "
     "afterward must not retroactively change an already-taken slice")
{
    const std::string source = "f() -> str { "
                               "  s = String(\"Axea\") "
                               "  sliced = s[0..4] "
                               "  s.append(\" Language\") "
                               "  return sliced "
                               "} "
                               "x = f()";
    EXPECT_EQ(toString(run(source)), "Axea");
}

TEST("Interpreter produces an empty str for a zero-length slice range")
{
    EXPECT_EQ(toString(run("x = \"hello\"[2..2]")), "");
}

TEST("Interpreter rejects an out-of-bounds slice range")
{
    EXPECT_THROWS(run("x = \"hi\"[0..5]"));
}

TEST("Interpreter rejects a slice range where start exceeds end")
{
    EXPECT_THROWS(run("x = \"hello\"[3..1]"));
}

TEST("Interpreter slices using runtime-computed bounds, not just integer literals")
{
    const std::string source = "n = 4 "
                               "date = \"2026-08-18\" "
                               "x = date[0..n]";
    EXPECT_EQ(toString(run(source)), "2026");
}

TEST("Interpreter parses str to i32, including a negative sign - unwrap_or reads Optional<i32>'s "
     "payload out (see docs/language/0052-optional.md)")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = \"42\".parse<i32>().unwrap_or(0)")), 42);
    EXPECT_EQ(std::get<std::int64_t>(run("x = \"-17\".parse<i32>().unwrap_or(0)")), -17);
    EXPECT_EQ(std::get<std::int64_t>(run("x = \"0\".parse<i32>().unwrap_or(1)")), 0);
}

TEST("Interpreter's parse<i32> yields None for invalid input - a real failure, not a silently "
     "returned fallback, matching the compiled backend's own identical choice")
{
    EXPECT_EQ(std::get<bool>(run("x = \"abc\".parse<i32>().is_none()")), true);
    EXPECT_EQ(std::get<bool>(run("x = \"\".parse<i32>().is_none()")), true);
    EXPECT_EQ(std::get<bool>(run("x = \"12abc\".parse<i32>().is_none()")), true);
    EXPECT_EQ(std::get<std::int64_t>(run("x = \"abc\".parse<i32>().unwrap_or(99)")), 99);
}

TEST("Interpreter parses str to i64, including a value that genuinely exceeds i32's own "
     "range (see docs/language/0051-numeric-widening.md)")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = \"123456789012\".parse<i64>().unwrap_or(0)")),
              123456789012);
    EXPECT_EQ(std::get<std::int64_t>(run("x = \"-999\".parse<i64>().unwrap_or(0)")), -999);
}

TEST("Interpreter parses str to f64 via a real strtod call, matching LlvmIrEmitter's own "
     "@axea.parse.f64 exactly")
{
    EXPECT_EQ(std::get<double>(run("x = \"3.14159\".parse<f64>().unwrap_or(0.0)")), 3.14159);
    EXPECT_EQ(std::get<double>(run("x = \"-2.5\".parse<f64>().unwrap_or(0.0)")), -2.5);
}

TEST("Interpreter's parse<f64> yields None for invalid input - the same real-failure contract "
     "parse<i32>/parse<bool> already established")
{
    EXPECT_EQ(std::get<bool>(run("x = \"not_a_number\".parse<f64>().is_none()")), true);
    EXPECT_EQ(std::get<bool>(run("x = \"3.14abc\".parse<f64>().is_none()")), true);
}

TEST("Interpreter parses str to bool, requiring an exact 'true'/'false' match, else None")
{
    EXPECT_EQ(std::get<bool>(run("x = \"true\".parse<bool>().unwrap_or(false)")), true);
    EXPECT_EQ(std::get<bool>(run("x = \"false\".parse<bool>().unwrap_or(true)")), false);
    EXPECT_EQ(std::get<bool>(run("x = \"TRUE\".parse<bool>().is_none()")), true);
    EXPECT_EQ(std::get<bool>(run("x = \"trueX\".parse<bool>().is_none()")), true);
    EXPECT_EQ(std::get<bool>(run("x = \"\".parse<bool>().is_none()")), true);
}

TEST("Interpreter's enum construction (both payload and bare no-payload variants) and 'match' "
     "work end to end, including real exhaustiveness (see docs/language/0064-enums.md)")
{
    const std::string source = "enum Shape { Circle(f64)  Rectangle(f64, f64)  Point } "
                               "area(s: Shape) -> f64 { "
                               "  return match s { "
                               "    Circle(r) => 3.14159 * r * r "
                               "    Rectangle(w, h) => w * h "
                               "    Point => 0.0 "
                               "  } "
                               "} "
                               "c = Shape.Circle(5.0) "
                               "r = Shape.Rectangle(3.0, 4.0) "
                               "p = Shape.Point "
                               "cArea = area(c) "
                               "rArea = area(r) "
                               "pArea = area(p) "
                               "x = cArea";
    auto results = runProgram(source);
    EXPECT_EQ(toString(results.at("c")), "Circle(5)");
    EXPECT_EQ(toString(results.at("r")), "Rectangle(3, 4)");
    EXPECT_EQ(toString(results.at("p")), "Point");
    const double cArea = std::get<double>(results.at("cArea"));
    EXPECT_TRUE(cArea > 78.53 && cArea < 78.55);
    EXPECT_EQ(std::get<double>(results.at("rArea")), 12.0);
    EXPECT_EQ(std::get<double>(results.at("pArea")), 0.0);
}

TEST("Interpreter's 'match' dispatches via a wildcard arm for every variant it doesn't name")
{
    const std::string source = "enum Shape { Circle(f64)  Rectangle(f64, f64)  Point } "
                               "describe(s: Shape) -> str { "
                               "  return match s { Circle(r) => \"circle\"  _ => \"other\" } "
                               "} "
                               "a = describe(Shape.Circle(1.0)) "
                               "b = describe(Shape.Point) "
                               "x = a";
    auto results = runProgram(source);
    EXPECT_EQ(toString(results.at("a")), "circle");
    EXPECT_EQ(toString(results.at("b")), "other");
}

TEST("Interpreter's enum value nested inside a collection and inside a struct field prints "
     "correctly, and print()/interpolation both accept an enum value directly")
{
    const std::string source = "enum Shape { Circle(f64)  Point } "
                               "struct Wrapper { s: Shape } "
                               "shapes = [Shape.Circle(1.0), Shape.Point] "
                               "w = Wrapper { s: Shape.Circle(2.0) } "
                               "interp = \"{Shape.Circle(3.0)}\" "
                               "x = shapes";
    auto results = runProgram(source);
    EXPECT_EQ(toString(results.at("shapes")), "[Circle(1), Point]");
    EXPECT_EQ(toString(results.at("w")), "Wrapper { s: Circle(2) }");
    EXPECT_EQ(toString(results.at("interp")), "Circle(3)");
}

TEST("Interpreter's Ok(x)/Err(e)/unwrap_or/is_ok/is_err work end to end, mirroring "
     "Optional<T>'s own toString/unwrap_or/is_some/is_none precedent (see "
     "docs/language/0063-result.md)")
{
    const std::string source = "divide(a: i32, b: i32) -> Result<i32, str> { "
                               "  if b == 0 { return Err(\"division by zero\") } "
                               "  return Ok(a / b) "
                               "} "
                               "good = divide(10, 2) "
                               "bad = divide(10, 0) "
                               "goodVal = good.unwrap_or(0 - 1) "
                               "badVal = bad.unwrap_or(0 - 1) "
                               "goodIsOk = good.is_ok() "
                               "badIsErr = bad.is_err() "
                               "x = goodVal";
    auto results = runProgram(source);
    EXPECT_EQ(toString(results.at("good")), "Ok(5)");
    EXPECT_EQ(toString(results.at("bad")), "Err(division by zero)");
    EXPECT_EQ(std::get<std::int64_t>(results.at("goodVal")), 5);
    EXPECT_EQ(std::get<std::int64_t>(results.at("badVal")), -1);
    EXPECT_EQ(std::get<bool>(results.at("goodIsOk")), true);
    EXPECT_EQ(std::get<bool>(results.at("badIsErr")), true);
}

TEST("Interpreter's '?' propagates Err(e) out of the enclosing function, preserving the exact "
     "error value, and never evaluates code after the failing '?' - mirrors "
     "docs/language/0052-optional.md's own None-propagation precedent for Optional<T>")
{
    const std::string source = "parseDigit(a: i32, b: i32) -> Result<i32, i32> { "
                               "  if b == 0 { return Err(0 - 1) } "
                               "  return Ok(a / b) "
                               "} "
                               "sumTwo(a: i32, b: i32, c: i32, d: i32) -> Result<i32, i32> { "
                               "  x = parseDigit(a, b)? "
                               "  y = parseDigit(c, d)? "
                               "  return Ok(x + y) "
                               "} "
                               "good = sumTwo(10, 2, 20, 4) "
                               "bad = sumTwo(10, 2, 20, 0) "
                               "x = good";
    auto results = runProgram(source);
    EXPECT_EQ(toString(results.at("good")), "Ok(10)");
    EXPECT_EQ(toString(results.at("bad")), "Err(-1)");
}

TEST("Interpreter dispatches '?' correctly between Optional<T> and Result<T,E> based on the "
     "operand's own runtime shape, inside functions returning each kind respectively")
{
    const std::string source = "asOptional(s: str) -> Optional<i32> { return s.parse<i32>() } "
                               "useOptional(s: str) -> Optional<i32> { x = asOptional(s)?  return "
                               "Some(x + 1) } "
                               "asResult(a: i32, b: i32) -> Result<i32, i32> { "
                               "  if b == 0 { return Err(0 - 1) } return Ok(a / b) "
                               "} "
                               "useResult(a: i32, b: i32) -> Result<i32, i32> { "
                               "  x = asResult(a, b)?  return Ok(x + 1) "
                               "} "
                               "o = useOptional(\"5\") "
                               "r = useResult(10, 2) "
                               "x = o";
    auto results = runProgram(source);
    EXPECT_EQ(toString(results.at("o")), "Some(6)");
    EXPECT_EQ(toString(results.at("r")), "Ok(6)");
}

TEST("Interpreter's Result<T,E> value nested inside a collection and inside a struct field "
     "prints correctly via the default field/element printer")
{
    const std::string source = "struct Wrapper { r: Result<i32, i32> } "
                               "good = Ok(1) "
                               "bad = Err(0 - 1) "
                               "results = [good, bad] "
                               "w = Wrapper { r: good } "
                               "x = results";
    auto results = runProgram(source);
    EXPECT_EQ(toString(results.at("results")), "[Ok(1), Err(-1)]");
    EXPECT_EQ(toString(results.at("w")), "Wrapper { r: Ok(1) }");
}

TEST("Interpreter parses a str slice result directly - date[..4].parse<i32>()")
{
    const std::string source = "date = \"2026-08-18\" "
                               "x = date[..4].parse<i32>().unwrap_or(0)";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 2026);
}

TEST("Interpreter parses a String, str-coerced the same way .append's own argument is")
{
    const std::string source = "s = String(\"123\") "
                               "x = s.parse<i32>().unwrap_or(0)";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 123);
}

TEST("Interpreter rejects parse<T>() for an unsupported target type")
{
    EXPECT_THROWS(run("x = \"5\".parse<str>()"));
}

TEST("Interpreter's str .length counts Unicode codepoints, not bytes - .bytes is the raw byte "
     "count (see docs/language/0047-unicode.md)")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = \"héllo\".length")), 5);
    EXPECT_EQ(std::get<std::int64_t>(run("x = \"héllo\".bytes")), 6);
    EXPECT_EQ(std::get<std::int64_t>(run("x = \"hello\".length")), 5);
    EXPECT_EQ(std::get<std::int64_t>(run("x = \"hello\".bytes")), 5);
}

TEST("Interpreter's String .length counts Unicode codepoints, .bytes is the raw byte count")
{
    const std::string source = "s = String(\"héllo\") "
                               "len = s.length "
                               "x = s.bytes";
    auto results = runProgram(source);
    EXPECT_EQ(std::get<std::int64_t>(results.at("len")), 5);
    EXPECT_EQ(std::get<std::int64_t>(results.at("x")), 6);
}

TEST("Interpreter's Buffer .length counts Unicode codepoints, .bytes is the raw byte count, "
     ".capacity is unaffected")
{
    const std::string source = "b = Buffer() "
                               "t = b.append(\"héllo\") "
                               "len = b.length "
                               "x = b.bytes";
    auto results = runProgram(source);
    EXPECT_EQ(std::get<std::int64_t>(results.at("len")), 5);
    EXPECT_EQ(std::get<std::int64_t>(results.at("x")), 6);
}

TEST("Interpreter counts multi-byte (4-byte) codepoints correctly - three rockets is length 3, "
     "bytes 12")
{
    const std::string source = "s = \"🚀🚀🚀\" "
                               "len = s.length "
                               "x = s.bytes";
    auto results = runProgram(source);
    EXPECT_EQ(std::get<std::int64_t>(results.at("len")), 3);
    EXPECT_EQ(std::get<std::int64_t>(results.at("x")), 12);
}

TEST("Interpreter's hand-implemented extern 'puts' writes its argument plus a trailing newline "
     "to stdout, matching real libc puts() - the one extern name the interpreter can actually "
     "execute (see docs/language/0048-ffi.md)")
{
    const std::string source = "extern c puts(text: cstr) "
                               "s = \"hello\" "
                               "called = puts(s.to_cstr())";

    std::ostringstream captured;
    std::streambuf* originalCout = std::cout.rdbuf(captured.rdbuf());
    runProgram(source);
    std::cout.rdbuf(originalCout);

    EXPECT_EQ(captured.str(), "hello\n");
}

TEST("Interpreter's .to_cstr() is a pure identity - cstr and str share the same underlying "
     "representation")
{
    EXPECT_EQ(toString(run("x = \"hello\".to_cstr()")), "hello");
}

TEST("Interpreter's .to_cstr() on a String returns its current content, str-coerced the same "
     "way .append's own argument is")
{
    const std::string source = "s = String(\"hello\") "
                               "x = s.to_cstr()";
    EXPECT_EQ(toString(run(source)), "hello");
}

TEST("Interpreter throws a clear error for an extern function with no hand-implemented "
     "behavior, unlike the compiled backend which can link against any correctly-declared "
     "extern c symbol")
{
    EXPECT_THROWS(run("extern c getpid() -> i32  x = getpid()"));
}

TEST("Interpreter's hand-implemented extern 'abs' matches real libc abs()")
{
    EXPECT_EQ(std::get<std::int64_t>(run("extern c abs(x: i32) -> i32  x = abs(0 - 42)")), 42);
    EXPECT_EQ(std::get<std::int64_t>(run("extern c abs(x: i32) -> i32  x = abs(7)")), 7);
}

TEST("Interpreter's print() writes space-separated arguments plus a trailing newline "
     "(see docs/language/Axea_Printing_Formatting.md)")
{
    const std::string source = "run() -> i32 { print(\"hello\", 1, true) return 0 } r = run()";

    std::ostringstream captured;
    std::streambuf* originalCout = std::cout.rdbuf(captured.rdbuf());
    runProgram(source);
    std::cout.rdbuf(originalCout);

    EXPECT_EQ(captured.str(), "hello 1 true\n");
}

TEST("Interpreter's write() writes space-separated arguments with no trailing newline")
{
    const std::string source = "run() -> i32 { write(\"a\") write(\"b\") return 0 } r = run()";

    std::ostringstream captured;
    std::streambuf* originalCout = std::cout.rdbuf(captured.rdbuf());
    runProgram(source);
    std::cout.rdbuf(originalCout);

    EXPECT_EQ(captured.str(), "ab");
}

TEST("Interpreter's print() with zero arguments writes just a newline")
{
    const std::string source = "run() -> i32 { print() return 0 } r = run()";

    std::ostringstream captured;
    std::streambuf* originalCout = std::cout.rdbuf(captured.rdbuf());
    runProgram(source);
    std::cout.rdbuf(originalCout);

    EXPECT_EQ(captured.str(), "\n");
}

TEST("Interpreter actually executes a bare top-level print(...)/write(...) call - not just "
     "parses it - via the new ExprStmt case in Interpreter::run's own top-level item loop "
     "(see docs/language/0049-printing-formatting.md's own Parsing follow-up)")
{
    const std::string source = "write(\"a\") print(\"b\")";

    std::ostringstream captured;
    std::streambuf* originalCout = std::cout.rdbuf(captured.rdbuf());
    runProgram(source);
    std::cout.rdbuf(originalCout);

    EXPECT_EQ(captured.str(), "ab\n");
}

TEST("Interpreter's print(...)/write(...) and interpolation now accept an Array/List/struct "
     "argument, via the TypeChecker's own widened isTextRepresentable (see "
     "docs/language/0054-collection-printing.md) - toString() itself needed no changes, "
     "already fully general")
{
    const std::string source = "struct Point { x: i32 } "
                               "run() -> i32 { "
                               "  arr = [1, 2, 3] "
                               "  p = Point { x: 5 } "
                               "  print(arr, p) "
                               "  s = \"arr={arr} p={p}\" "
                               "  print(s) "
                               "  return 0 "
                               "} "
                               "r = run()";

    std::ostringstream captured;
    std::streambuf* originalCout = std::cout.rdbuf(captured.rdbuf());
    runProgram(source);
    std::cout.rdbuf(originalCout);

    EXPECT_EQ(captured.str(), "[1, 2, 3] Point { x: 5 }\narr=[1, 2, 3] p=Point { x: 5 }\n");
}

TEST("Interpreter evaluates a string interpolation expression by concatenating each piece's "
     "toString(), reusing the same generic stringifier every other printable type already has")
{
    const std::string source = "name = \"Ada\" age = 30 "
                               "x = \"{name} is {age} years old\"";
    EXPECT_EQ(toString(run(source)), "Ada is 30 years old");
}

TEST("Interpreter evaluates an arithmetic expression inside an interpolation span")
{
    EXPECT_EQ(toString(run("age = 30 x = \"next year: {age + 1}\"")), "next year: 31");
}

TEST("Interpreter evaluates a string interpolation span containing its own nested string "
     "literal (e.g. `.join(\",\")`'s own separator argument) - see "
     "docs/language/0049-printing-formatting.md's own follow-up")
{
    const std::string source = "numbers: List<i32> = List<i32>() "
                               "a = numbers.push(1) "
                               "b = numbers.push(2) "
                               "x = \"nums: {numbers.join(\",\")}\"";
    EXPECT_EQ(toString(run(source)), "nums: 1,2");
}

TEST("Interpreter treats '{{' and '}}' as literal escaped braces in an interpolated string")
{
    EXPECT_EQ(toString(run("age = 30 x = \"{{escaped}} and {age}\"")), "{escaped} and 30");
}

TEST("Interpreter interpolates bool and char values via the same UTF-8-aware toString() every "
     "other printable type uses")
{
    EXPECT_EQ(toString(run("ok = true x = \"bool: {ok}\"")), "bool: true");
    EXPECT_EQ(toString(run("c = 'z' x = \"char: {c}\"")), "char: z");
}

TEST("Interpreter's print()/write() and interpolation both stringify a plain str literal with no "
     "quotes added")
{
    const std::string source = "run() -> i32 { print(\"plain\") return 0 } r = run()";

    std::ostringstream captured;
    std::streambuf* originalCout = std::cout.rdbuf(captured.rdbuf());
    runProgram(source);
    std::cout.rdbuf(originalCout);

    EXPECT_EQ(captured.str(), "plain\n");
}

TEST("Interpreter slices a fixed-size array into a fresh List<T> - indexing and .length both "
     "work on the result, same as any other List (see "
     "docs/language/0050-collection-join-and-slicing.md)")
{
    const std::string source = "f() -> i32 { "
                               "  numbers = [10, 20, 30, 40] "
                               "  sliced = numbers[1..3] "
                               "  return sliced[0] + sliced[1] + sliced.length "
                               "} "
                               "x = f()"; // 20 + 30 + 2 = 52
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 52);
}

TEST("Interpreter slices a List<T> into another fresh List<T>, unaffected by later mutation of "
     "the source")
{
    const std::string source = "f() -> i32 { "
                               "  numbers = List<i32>() "
                               "  numbers.push(1) "
                               "  numbers.push(2) "
                               "  numbers.push(3) "
                               "  sliced = numbers[..] "
                               "  numbers.push(4) "
                               "  return sliced.length "
                               "} "
                               "x = f()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 3);
}

TEST("Interpreter defaults a missing slice start to 0 and a missing end to the collection's own "
     "length, matching str slicing's own precedent")
{
    const std::string source = "f() -> i32 { "
                               "  numbers = [1, 2, 3] "
                               "  whole = numbers[..] "
                               "  return whole.length "
                               "} "
                               "x = f()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 3);
}

TEST("Interpreter throws on an out-of-bounds Array/List slice range, matching str slicing's own "
     "runtime bounds check")
{
    EXPECT_THROWS(runProgram("f() -> i32 { numbers = [1, 2, 3] bad = numbers[1..10] return "
                             "bad.length } x = f()"));
}

TEST("Interpreter's .join(separator) stringifies each element via the same generic toString() "
     "print()/interpolation already use, joined with separator")
{
    const std::string source = "f() -> String { "
                               "  numbers = [1, 2, 3] "
                               "  return numbers.join(\",\") "
                               "} "
                               "x = f()";
    EXPECT_EQ(toString(run(source)), "1,2,3");
}

TEST("Interpreter's .join() on an empty Array/List returns an empty String")
{
    const std::string source = "f() -> i32 { "
                               "  numbers = [1, 2, 3] "
                               "  empty = numbers[2..2] "
                               "  joined = empty.join(\",\") "
                               "  return joined.length "
                               "} "
                               "x = f()";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 0);
}

TEST("Interpreter's .join() works on a List<str>, joining each string with the separator")
{
    const std::string source = "f() -> String { "
                               "  names = List<str>() "
                               "  names.push(\"ada\") "
                               "  names.push(\"grace\") "
                               "  return names.join(\", \") "
                               "} "
                               "x = f()";
    EXPECT_EQ(toString(run(source)), "ada, grace");
}

TEST("Interpreter applies a numeric format spec's zero-padded width to an i32 interpolation "
     "span (see docs/language/0055-numeric-format-specs.md)")
{
    EXPECT_EQ(toString(run("n = 42 x = \"{n:05}\"")), "00042");
}

TEST("Interpreter applies a precision format spec to an f64 interpolation span, rounding like "
     "printf's own %.Nf")
{
    EXPECT_EQ(toString(run("pi = 3.14159 x = \"{pi:.2}\"")), "3.14");
}

TEST("Interpreter applies x/X/b/o radix format specs to an i32 interpolation span")
{
    EXPECT_EQ(toString(run("n = 42 x = \"{n:x}\"")), "2a");
    EXPECT_EQ(toString(run("n = 42 x = \"{n:X}\"")), "2A");
    EXPECT_EQ(toString(run("n = 42 x = \"{n:b}\"")), "101010");
    EXPECT_EQ(toString(run("n = 42 x = \"{n:o}\"")), "52");
}

TEST("Interpreter formats a negative i32 radix conversion by reinterpreting its full 64-bit "
     "two's-complement bit pattern, matching the compiled backend's own choice (see "
     "LlvmIrEmitter.hpp's registerFormatRuntime comment)")
{
    EXPECT_EQ(toString(run("n = 100 - 142 x = \"{n:x}\"")), "ffffffffffffffd6");
}

TEST("Interpreter's binary format spec floors the digit count at 1 for a zero value, rather "
     "than printing an empty string")
{
    EXPECT_EQ(toString(run("n = 0 x = \"{n:b}\"")), "0");
    EXPECT_EQ(toString(run("n = 0 x = \"{n:08b}\"")), "00000000");
}

TEST("Interpreter's plain width format spec (no zero-pad, no type char) space-pads an i32 "
     "interpolation span")
{
    EXPECT_EQ(toString(run("n = 5 x = \"[{n:10}]\"")), "[         5]");
}

TEST("Interpreter applies a numeric format spec to an i64 interpolation span the same way it "
     "does for i32")
{
    EXPECT_EQ(toString(run("n: i64 = 123456789012i64 x = \"{n:015}\"")), "000123456789012");
    EXPECT_EQ(toString(run("n: i64 = 123456789012i64 x = \"{n:x}\"")), "1cbe991a14");
}

TEST("Interpreter's print() prints a slice<T>-typed parameter with the same bracket format as "
     "an Array (see docs/language/0056-slice-printing.md)")
{
    const std::string source =
        "f(s: slice<i32>) -> i32 { print(s) return 0 } arr = [1, 2, 3] r = f(arr)";

    std::ostringstream captured;
    std::streambuf* originalCout = std::cout.rdbuf(captured.rdbuf());
    runProgram(source);
    std::cout.rdbuf(originalCout);

    EXPECT_EQ(captured.str(), "[1, 2, 3]\n");
}

TEST("Interpreter interpolates a slice<T>-typed parameter into a string, same bracket format "
     "as print()")
{
    const std::string source = "f(s: slice<i32>) -> String { return \"vals: {s}\" } "
                               "arr = [4, 5] x = f(arr)";
    EXPECT_EQ(toString(run(source)), "vals: [4, 5]");
}

TEST("Interpreter's .join() works on a slice<T>-typed parameter, same as Array/List")
{
    const std::string source = "f(s: slice<i32>) -> String { return s.join(\"-\") } "
                               "arr = [1, 2, 3] x = f(arr)";
    EXPECT_EQ(toString(run(source)), "1-2-3");
}

TEST("Interpreter prints/joins a slice<T> of struct elements, each stringified via the same "
     "@axea.tostring.<Name>-equivalent toString() print()/join() already use for Array")
{
    const std::string source = "struct Point { x: i32 } "
                               "f(s: slice<Point>) -> String { return s.join(\", \") } "
                               "pts = [Point{x:1}, Point{x:2}] x = f(pts)";
    EXPECT_EQ(toString(run(source)), "Point { x: 1 }, Point { x: 2 }");
}

TEST("Interpreter left/right/center-aligns a str interpolation span to a given width, padding "
     "with spaces (see docs/language/0057-alignment.md)")
{
    EXPECT_EQ(toString(run("name = \"hi\" x = \"[{name:<10}]\"")), "[hi        ]");
    EXPECT_EQ(toString(run("name = \"hi\" x = \"[{name:>10}]\"")), "[        hi]");
    EXPECT_EQ(toString(run("name = \"hi\" x = \"[{name:^10}]\"")), "[    hi    ]");
}

TEST("Interpreter's center alignment puts the extra space on the right for an odd padding "
     "amount")
{
    EXPECT_EQ(toString(run("name = \"hi\" x = \"[{name:^11}]\"")), "[    hi     ]");
}

TEST("Interpreter's alignment never truncates - a value already at least as wide as the "
     "target width passes through unchanged")
{
    EXPECT_EQ(toString(run("name = \"hello world\" x = \"[{name:<5}]\"")), "[hello world]");
}

TEST("Interpreter aligns a numeric interpolation span (bool/i32), not just str - alignment "
     "applies to any text-representable type")
{
    EXPECT_EQ(toString(run("ok = true x = \"[{ok:<10}]\"")), "[true      ]");
    EXPECT_EQ(toString(run("n = 5 x = \"[{n:>6}]\"")), "[     5]");
}

TEST("Interpreter combines alignment with a radix conversion, padding the resulting hex text "
     "(the core-text-then-pad path reuses formatValue's own hex conversion unchanged)")
{
    EXPECT_EQ(toString(run("n = 255 x = \"[{n:>10x}]\"")), "[        ff]");
    EXPECT_EQ(toString(run("n = 255 x = \"[{n:<10x}]\"")), "[ff        ]");
}

TEST("Interpreter combines alignment with a float precision, matching the source doc's own "
     "{user.score:>8.2} example")
{
    EXPECT_EQ(toString(run("pi = 3.14159 x = \"[{pi:>8.2}]\"")), "[    3.14]");
}

TEST("Interpreter's self-documenting '{expr=}' prints the raw source text, '=', then the "
     "value's own normal representation, matching the source doc's own worked example (see "
     "docs/language/0058-debug-formatting.md)")
{
    EXPECT_EQ(toString(run("n = 42 x = \"{n=}\"")), "n=42");
}

TEST("Interpreter's self-doc marker echoes the raw expression source text verbatim, not a "
     "re-rendering of the parsed AST - e.g. 'age + 1', not '(age + 1)' or similar")
{
    EXPECT_EQ(toString(run("age = 30 x = \"{age + 1=}\"")), "age + 1=31");
}

TEST("Interpreter's debug format '{value:?}' is identical to the unformatted case for every "
     "type except str/String, which get wrapped in quotes")
{
    EXPECT_EQ(toString(run("n = 42 x = \"{n:?}\"")), "42");
    EXPECT_EQ(toString(run("ok = true x = \"{ok:?}\"")), "true");
    EXPECT_EQ(toString(run("name = \"Ada\" x = \"{name:?}\"")), "\"Ada\"");
}

TEST("Interpreter's debug format quotes an owned String the same way it quotes a bare str")
{
    const std::string source = "f() -> String { "
                               "  greeting: String = \"hi\" "
                               "  return \"{greeting:?}\" "
                               "} "
                               "x = f()";
    EXPECT_EQ(toString(run(source)), "\"hi\"");
}

TEST("Interpreter's debug format on a struct is identical to the unformatted case - no "
     "Display/Debug distinction exists yet for struct printing (see "
     "docs/language/0058-debug-formatting.md's own Known Imprecision)")
{
    const std::string source = "struct Point { x: i32 } "
                               "p = Point { x: 1 } "
                               "normal = \"{p}\" "
                               "debug = \"{p:?}\"";
    auto bindings = runProgram(source);
    EXPECT_EQ(toString(bindings.at("normal")), toString(bindings.at("debug")));
}

TEST("Interpreter combines a self-doc prefix with a debug spec: '{s=:?}' prints the raw "
     "source text, '=', then the quoted debug representation")
{
    EXPECT_EQ(toString(run("s = \"hi\" x = \"{s=:?}\"")), "s=\"hi\"");
}

TEST("Interpreter dispatches to a user's impl Display for a struct interpolated inside a "
     "string, instead of the default per-field printer (see "
     "docs/language/0062-display-trait.md)")
{
    const std::string source =
        "struct Point { x: i32  y: i32 } "
        "impl Display for Point { "
        "  format(self, buf: Buffer) { buf.write(\"({self.x}, {self.y})\") } "
        "} "
        "p = Point { x: 10, y: 20 } "
        "x = \"Position: {p}\"";
    EXPECT_EQ(toString(run(source)), "Position: (10, 20)");
}

TEST("Interpreter dispatches to a user's impl Display for a bare print() struct argument")
{
    const std::string source =
        "struct Point { x: i32  y: i32 } "
        "impl Display for Point { "
        "  format(self, buf: Buffer) { buf.write(\"({self.x}, {self.y})\") } "
        "} "
        "run() -> i32 { p = Point { x: 1, y: 2 }  print(p)  return 0 } "
        "r = run()";

    std::ostringstream captured;
    std::streambuf* originalCout = std::cout.rdbuf(captured.rdbuf());
    runProgram(source);
    std::cout.rdbuf(originalCout);

    EXPECT_EQ(captured.str(), "(1, 2)\n");
}

TEST("Interpreter dispatches to a user's impl Display for a struct nested inside another "
     "struct's own default field printer")
{
    const std::string source =
        "struct Point { x: i32  y: i32 } "
        "struct Line { a: Point  b: Point } "
        "impl Display for Point { "
        "  format(self, buf: Buffer) { buf.write(\"({self.x}, {self.y})\") } "
        "} "
        "line = Line { a: Point { x: 1, y: 2 }, b: Point { x: 3, y: 4 } } "
        "x = \"{line}\"";
    EXPECT_EQ(toString(run(source)), "Line { a: (1, 2), b: (3, 4) }");
}

TEST("Interpreter dispatches to a user's impl Display for a struct nested inside an array")
{
    const std::string source =
        "struct Point { x: i32  y: i32 } "
        "impl Display for Point { "
        "  format(self, buf: Buffer) { buf.write(\"({self.x}, {self.y})\") } "
        "} "
        "points = [Point { x: 1, y: 2 }, Point { x: 3, y: 4 }] "
        "x = \"{points}\"";
    EXPECT_EQ(toString(run(source)), "[(1, 2), (3, 4)]");
}

TEST("Interpreter dispatches to Display for a struct's top-level auto-printed binding - the "
     "one call site that happens *after* Interpreter::run() has already returned, still while "
     "the Interpreter instance itself is alive")
{
    const std::string source =
        "struct Point { x: i32  y: i32 } "
        "impl Display for Point { "
        "  format(self, buf: Buffer) { buf.write(\"({self.x}, {self.y})\") } "
        "} "
        "p = Point { x: 7, y: 8 }";

    Lexer lexer(source);
    Parser parser(lexer.lex());
    auto program = parser.parseProgram();
    Interpreter interpreter;
    interpreter.run(program);
    EXPECT_EQ(toString(interpreter.variables().at("p")), "(7, 8)");
}

TEST("Interpreter falls back to the default per-field printer for a struct type with no "
     "registered impl Display, even when other structs in the same program have one")
{
    const std::string source =
        "struct Point { x: i32  y: i32 } "
        "struct Other { n: i32 } "
        "impl Display for Point { "
        "  format(self, buf: Buffer) { buf.write(\"({self.x}, {self.y})\") } "
        "} "
        "x = Other { n: 5 }";
    EXPECT_EQ(toString(run(source)), "Other { n: 5 }");
}

TEST("Interpreter implicitly wraps a plain value into a union-typed call argument, declared "
     "local, and return, with no wrapper syntax, and 'match' dispatches on it by each "
     "alternative's own type name (see docs/language/0065-unions.md)")
{
    const std::string source = "f(x: i32 | str) -> str { "
                               "  return match x { i32(n) => \"number\"  str(s) => \"string\" } "
                               "} "
                               "a = f(5) "
                               "b = f(\"hi\") "
                               "w: i32 | str = 5";
    auto results = runProgram(source);
    EXPECT_EQ(toString(results.at("a")), "number");
    EXPECT_EQ(toString(results.at("b")), "string");
    EXPECT_EQ(toString(results.at("w")), "i32(5)");
}

TEST("Interpreter forwards an already-union-typed value through another union-typed boundary "
     "without re-wrapping it")
{
    const std::string source = "f(x: i32 | str) -> i32 | str { return x } "
                               "g(x: i32 | str) -> i32 | str { return f(x) } "
                               "a = g(5) "
                               "b = g(\"hi\")";
    auto results = runProgram(source);
    EXPECT_EQ(toString(results.at("a")), "i32(5)");
    EXPECT_EQ(toString(results.at("b")), "str(hi)");
}

TEST("Interpreter's union value wraps a struct alternative by its own type name")
{
    const std::string source = "struct Point { x: i32  y: i32 } "
                               "f(v: Point | i32) -> str { "
                               "  return match v { Point(p) => \"point\"  i32(n) => \"number\" } "
                               "} "
                               "p = Point { x: 1, y: 2 } "
                               "a = f(p) "
                               "b = f(5)";
    auto results = runProgram(source);
    EXPECT_EQ(toString(results.at("a")), "point");
    EXPECT_EQ(toString(results.at("b")), "number");
}

TEST("Interpreter calls a closure literal assigned to a declared local (see "
     "docs/language/0067-closures.md)")
{
    const std::string source =
        "add: fn(i32, i32) -> i32 = fn(x: i32, y: i32) -> i32 { return x + y } "
        "y = add(2, 3)";
    auto results = runProgram(source);
    EXPECT_EQ(std::get<std::int64_t>(results.at("y")), 5);
}

TEST("Interpreter's closure closes over an enclosing function's own param by value - returned, "
     "and called later, each captured value stays independent of any other closure's own copy")
{
    const std::string source = "makeAdder(base: i32) -> fn(i32) -> i32 { "
                               "  return fn(x: i32) -> i32 { return x + base } "
                               "} "
                               "add5 = makeAdder(5) "
                               "add10 = makeAdder(10) "
                               "a = add5(1) "
                               "b = add10(1)";
    auto results = runProgram(source);
    EXPECT_EQ(std::get<std::int64_t>(results.at("a")), 6);
    EXPECT_EQ(std::get<std::int64_t>(results.at("b")), 11);
}

TEST("Interpreter calls a closure-typed parameter - a higher-order function")
{
    const std::string source = "apply(f: fn(i32) -> i32, x: i32) -> i32 { return f(x) } "
                               "doubler: fn(i32) -> i32 = fn(x: i32) -> i32 { return x * 2 } "
                               "tripler: fn(i32) -> i32 = fn(x: i32) -> i32 { return x * 3 } "
                               "a = apply(doubler, 5) "
                               "b = apply(tripler, 5)";
    auto results = runProgram(source);
    EXPECT_EQ(std::get<std::int64_t>(results.at("a")), 10);
    EXPECT_EQ(std::get<std::int64_t>(results.at("b")), 15);
}

TEST("Interpreter wraps a bare top-level function name into a real closure value at each of the "
     "three boundaries that need it - call argument, declared-local assignment, and return (see "
     "docs/language/0067-closures.md's implicit function-reference-to-closure coercion)")
{
    const std::string source = "double(x: i32) -> i32 { return x * 2 } "
                               "apply(f: fn(i32) -> i32, x: i32) -> i32 { return f(x) } "
                               "getDouble() -> fn(i32) -> i32 { return double } "
                               "run() -> i32 { "
                               "  d: fn(i32) -> i32 = double "
                               "  a = apply(double, 5) "
                               "  b = d(7) "
                               "  g = getDouble() "
                               "  c = g(9) "
                               "  return a + b + c "
                               "} "
                               "y = run()";
    auto results = runProgram(source);
    EXPECT_EQ(std::get<std::int64_t>(results.at("y")), 42);
}

TEST("Interpreter's closure captures a struct-typed local by move, still readable through the "
     "captured copy after the closure is created")
{
    const std::string source = "struct Point { x: i32  y: i32 } "
                               "run() -> i32 { "
                               "  p = Point { x: 3, y: 4 } "
                               "  sum: fn() -> i32 = fn() -> i32 { return p.x + p.y } "
                               "  return sum() "
                               "} "
                               "y = run()";
    auto results = runProgram(source);
    EXPECT_EQ(std::get<std::int64_t>(results.at("y")), 7);
}

TEST("Interpreter computes a self-referential (recursive) closure correctly - no new syntax, "
     "`f`'s own name resolves to the closure being constructed even from inside its own body "
     "(see docs/language/0067-closures.md)")
{
    const std::string source = "run() -> i32 { "
                               "  fact: fn(i32) -> i32 = fn(n: i32) -> i32 { "
                               "    if n <= 1 { return 1 } "
                               "    return n * fact(n - 1) "
                               "  } "
                               "  return fact(5) "
                               "} "
                               "y = run()";
    auto results = runProgram(source);
    EXPECT_EQ(std::get<std::int64_t>(results.at("y")), 120);
}

TEST("Interpreter's self-referential closure also works with no declared type on its own "
     "binding, and alongside an ordinary capture from the enclosing scope")
{
    const std::string source = "run() -> i32 { "
                               "  base = 0 "
                               "  fib = fn(n: i32) -> i32 { "
                               "    if n <= 1 { return n + base } "
                               "    return fib(n - 1) + fib(n - 2) "
                               "  } "
                               "  return fib(10) "
                               "} "
                               "y = run()";
    auto results = runProgram(source);
    EXPECT_EQ(std::get<std::int64_t>(results.at("y")), 55);
}

TEST("Interpreter calls a closure with a real struct-typed *parameter* (as opposed to a capture) "
     "- passed in fresh at each call, not captured at the point the literal was created (see "
     "docs/language/0067-closures.md)")
{
    const std::string source = "struct Point { x: i32  y: i32 } "
                               "sum: fn(Point) -> i32 = fn(p: Point) -> i32 { return p.x + p.y } "
                               "a = sum(Point { x: 3, y: 4 }) "
                               "b = sum(Point { x: 10, y: 20 })";
    auto results = runProgram(source);
    EXPECT_EQ(std::get<std::int64_t>(results.at("a")), 7);
    EXPECT_EQ(std::get<std::int64_t>(results.at("b")), 30);
}
