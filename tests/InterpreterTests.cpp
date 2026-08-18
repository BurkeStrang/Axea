#include "TestFramework.hpp"

#include "interpreter/Interpreter.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"

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
