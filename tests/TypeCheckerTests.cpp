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

TEST("TypeChecker accepts push_front/push_back/pop_front/pop_back/.length on a LinkedList<T>")
{
    check("f() -> i32 { "
          "  s = LinkedList<i32>() "
          "  s.push_front(4) "
          "  s.push_back(5) "
          "  front = s.pop_front() "
          "  back = s.pop_back() "
          "  return front + back + s.length "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects 'push_front' with the wrong element type on a LinkedList<T>")
{
    EXPECT_THROWS(check("f() { s = LinkedList<i32>()  s.push_front(true) }"));
}

TEST("TypeChecker rejects 'pop_back' with arguments")
{
    EXPECT_THROWS(check("f() -> i32 { s = LinkedList<i32>()  return s.pop_back(1) }"));
}

TEST("TypeChecker rejects an unknown method on a LinkedList<T>")
{
    EXPECT_THROWS(check("f() { s = LinkedList<i32>()  s.size() }"));
}

TEST("TypeChecker accepts LinkedList<T> as a parameter, return type, and local declared type")
{
    check("build() -> LinkedList<i32> { "
          "  x: LinkedList<i32> = LinkedList<i32>() "
          "  return x "
          "} "
          "use(s: LinkedList<i32>) -> i32 { return s.length } "
          "n = build() "
          "y = use(n)");
}

TEST("TypeChecker rejects LinkedList<T> as a struct field type")
{
    EXPECT_THROWS(check("struct Wrapper { items: LinkedList<i32> }"));
}

TEST("TypeChecker accepts push_front/push_back/pop_front/pop_back/.length/[i] on a Deque<T>")
{
    check("f() -> i32 { "
          "  d = Deque<i32>() "
          "  d.push_front(4) "
          "  d.push_back(5) "
          "  front = d.pop_front() "
          "  back = d.pop_back() "
          "  d.push_back(9) "
          "  mid = d[0] "
          "  return front + back + mid + d.length "
          "} "
          "x = f()");
}

TEST("TypeChecker accepts index-assignment into a Deque<T>")
{
    check("f() { d = Deque<i32>()  d.push_back(1)  d[0] = 99 }");
}

TEST("TypeChecker rejects a non-i32 index into a Deque<T>")
{
    EXPECT_THROWS(check("f() -> i32 { d = Deque<i32>()  d.push_back(1)  return d[true] }"));
}

TEST("TypeChecker rejects 'push_front' with the wrong element type on a Deque<T>")
{
    EXPECT_THROWS(check("f() { d = Deque<i32>()  d.push_front(true) }"));
}

TEST("TypeChecker rejects an unknown method on a Deque<T>")
{
    EXPECT_THROWS(check("f() { d = Deque<i32>()  d.size() }"));
}

TEST("TypeChecker accepts Deque<T> as a parameter, return type, and local declared type")
{
    check("build() -> Deque<i32> { "
          "  x: Deque<i32> = Deque<i32>() "
          "  return x "
          "} "
          "use(d: Deque<i32>) -> i32 { return d.length } "
          "n = build() "
          "y = use(n)");
}

TEST("TypeChecker rejects Deque<T> as a struct field type")
{
    EXPECT_THROWS(check("struct Wrapper { items: Deque<i32> }"));
}

TEST("TypeChecker accepts enqueue/dequeue/.length on a Queue<T>")
{
    check("f() -> i32 { "
          "  q = Queue<i32>() "
          "  q.enqueue(4) "
          "  q.enqueue(5) "
          "  first = q.dequeue() "
          "  return first + q.length "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects indexing into a Queue<T> - deliberately not indexable, unlike Deque<T> "
     "(communicate intent, see docs/language/0038-queues.md)")
{
    EXPECT_THROWS(check("f() -> i32 { q = Queue<i32>()  q.enqueue(1)  return q[0] }"));
}

TEST("TypeChecker rejects 'enqueue' with the wrong element type on a Queue<T>")
{
    EXPECT_THROWS(check("f() { q = Queue<i32>()  q.enqueue(true) }"));
}

TEST("TypeChecker rejects 'dequeue' with arguments")
{
    EXPECT_THROWS(check("f() -> i32 { q = Queue<i32>()  return q.dequeue(1) }"));
}

TEST("TypeChecker rejects an unknown method on a Queue<T>")
{
    EXPECT_THROWS(check("f() { q = Queue<i32>()  q.size() }"));
}

TEST("TypeChecker accepts Queue<T> as a parameter, return type, and local declared type")
{
    check("build() -> Queue<i32> { "
          "  x: Queue<i32> = Queue<i32>() "
          "  return x "
          "} "
          "use(q: Queue<i32>) -> i32 { return q.length } "
          "n = build() "
          "y = use(n)");
}

TEST("TypeChecker rejects Queue<T> as a struct field type")
{
    EXPECT_THROWS(check("struct Wrapper { items: Queue<i32> }"));
}

TEST("TypeChecker accepts push/pop/peek/.length on a PriorityQueue<T>")
{
    check("f() -> i32 { "
          "  q = PriorityQueue<i32>() "
          "  q.push(4) "
          "  q.push(5) "
          "  top = q.peek() "
          "  smallest = q.pop() "
          "  return top + smallest + q.length "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects 'push' with the wrong element type on a PriorityQueue<T>")
{
    EXPECT_THROWS(check("f() { q = PriorityQueue<i32>()  q.push(true) }"));
}

TEST("TypeChecker rejects 'peek' with arguments on a PriorityQueue<T>")
{
    EXPECT_THROWS(check("f() -> i32 { q = PriorityQueue<i32>()  return q.peek(1) }"));
}

TEST("TypeChecker rejects an unknown method on a PriorityQueue<T>")
{
    EXPECT_THROWS(check("f() { q = PriorityQueue<i32>()  q.size() }"));
}

TEST("TypeChecker rejects indexing into a PriorityQueue<T> - deliberately not indexable "
     "(communicate intent, see docs/language/0039-priority-queues.md)")
{
    EXPECT_THROWS(check("f() -> i32 { q = PriorityQueue<i32>()  q.push(1)  return q[0] }"));
}

TEST("TypeChecker accepts PriorityQueue<T> as a parameter, return type, and local declared type")
{
    check("build() -> PriorityQueue<i32> { "
          "  x: PriorityQueue<i32> = PriorityQueue<i32>() "
          "  return x "
          "} "
          "use(q: PriorityQueue<i32>) -> i32 { return q.length } "
          "n = build() "
          "y = use(n)");
}

TEST("TypeChecker rejects PriorityQueue<T> as a struct field type")
{
    EXPECT_THROWS(check("struct Wrapper { items: PriorityQueue<i32> }"));
}

TEST("TypeChecker rejects a non-i32 element type on a PriorityQueue<T> - no other type is "
     "comparable yet (see docs/language/0039-priority-queues.md)")
{
    EXPECT_THROWS(check("q = PriorityQueue<bool>()"));
}

TEST("TypeChecker rejects a non-i32 element type on a PriorityQueue<T> parameter")
{
    EXPECT_THROWS(check("f(q: PriorityQueue<bool>) {}"));
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

TEST("TypeChecker accepts set/get/contains/remove/.length on a SortedMap<i32,i32>")
{
    check("f() -> i32 { "
          "  m = SortedMap<i32,i32>() "
          "  m.set(1, 100) "
          "  m.set(2, 200) "
          "  before = m.contains(2) "
          "  m.remove(2) "
          "  after = m.contains(2) "
          "  removedDelta = if before { 10 } else { 0 } "
          "  keptDelta = if after { 1 } else { 0 } "
          "  return m.get(1) + m.length * 1000 + removedDelta + keptDelta "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects a non-i32 key type on SortedMap<K,V> - no other type is comparable "
     "yet (see docs/language/0040-sorted-maps.md)")
{
    EXPECT_THROWS(check("m = SortedMap<bool,i32>()"));
    EXPECT_THROWS(check("m = SortedMap<str,i32>()"));
}

TEST("TypeChecker accepts arbitrary V (struct, array, List) on a SortedMap<i32,V> - only K is "
     "restricted to i32, V has no such requirement, mirroring Map<K,V>'s own V")
{
    check("struct Point { x: i32 } "
          "f() { "
          "  m = SortedMap<i32,Point>() "
          "  m.set(1, Point { x: 1 }) "
          "  l = SortedMap<i32,List<i32>>() "
          "  l.set(1, List<i32>()) "
          "}");
}

TEST("TypeChecker's SortedMap<K,V>.get() returns V's real resolved type, not always i32")
{
    check("struct Point { x: i32 } "
          "f() -> i32 { "
          "  m = SortedMap<i32,Point>() "
          "  m.set(1, Point { x: 42 }) "
          "  p = m.get(1) "
          "  return p.x "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects an unknown method on a SortedMap<K,V>")
{
    EXPECT_THROWS(check("f() { m = SortedMap<i32,i32>()  m.size() }"));
}

TEST("TypeChecker rejects indexing into a SortedMap<K,V> - deliberately not indexable, no "
     "`[key]` syntax or `for`-in iteration this phase (see docs/language/0040-sorted-maps.md)")
{
    EXPECT_THROWS(check("f() -> i32 { m = SortedMap<i32,i32>()  return m[0] }"));
}

TEST("TypeChecker accepts SortedMap<i32,i32> as a parameter, return type, and local declared "
     "type")
{
    check("build() -> SortedMap<i32,i32> { "
          "  x: SortedMap<i32,i32> = SortedMap<i32,i32>() "
          "  return x "
          "} "
          "use(m: SortedMap<i32,i32>) -> i32 { return m.length } "
          "n = build() "
          "y = use(n)");
}

TEST("TypeChecker rejects SortedMap<K,V> as a struct field type")
{
    EXPECT_THROWS(check("struct Wrapper { entries: SortedMap<i32,i32> }"));
}

TEST("TypeChecker accepts add/contains/remove/.length on a SortedSet<i32>")
{
    check("f() -> i32 { "
          "  s = SortedSet<i32>() "
          "  s.add(5) "
          "  s.add(6) "
          "  before = s.contains(6) "
          "  s.remove(6) "
          "  after = s.contains(6) "
          "  removedDelta = if before { 10 } else { 0 } "
          "  keptDelta = if after { 1 } else { 0 } "
          "  return s.length * 1000 + removedDelta + keptDelta "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects a non-i32 element type on SortedSet<T> - no other type is comparable "
     "yet (see docs/language/0041-sorted-sets.md)")
{
    EXPECT_THROWS(check("s = SortedSet<bool>()"));
    EXPECT_THROWS(check("s = SortedSet<str>()"));
}

TEST("TypeChecker rejects an unknown method on a SortedSet<T>")
{
    EXPECT_THROWS(check("f() { s = SortedSet<i32>()  s.push(1) }"));
}

TEST("TypeChecker rejects indexing into a SortedSet<T>")
{
    EXPECT_THROWS(check("f() -> i32 { s = SortedSet<i32>()  return s[0] }"));
}

TEST("TypeChecker accepts SortedSet<i32> as a parameter, return type, and local declared type")
{
    check("build() -> SortedSet<i32> { "
          "  x: SortedSet<i32> = SortedSet<i32>() "
          "  return x "
          "} "
          "use(s: SortedSet<i32>) -> i32 { return s.length } "
          "n = build() "
          "y = use(n)");
}

TEST("TypeChecker rejects SortedSet<T> as a struct field type")
{
    EXPECT_THROWS(check("struct Wrapper { items: SortedSet<i32> }"));
}

TEST("TypeChecker accepts String(text) construction and .append/.length")
{
    check("f() -> i32 { "
          "  s = String(\"Axea\") "
          "  s.append(\" Language\") "
          "  return s.length "
          "} "
          "x = f()");
}

TEST("TypeChecker accepts String(anotherString) - String lends itself as str-coercible too")
{
    check("f() { "
          "  a = String(\"a\") "
          "  b = String(a) "
          "}");
}

TEST("TypeChecker rejects String(...) with a non-str-coercible argument")
{
    EXPECT_THROWS(check("x = String(5)"));
    EXPECT_THROWS(check("x = String(true)"));
}

TEST("TypeChecker rejects 'append' with a non-str-coercible argument")
{
    EXPECT_THROWS(check("f() { s = String(\"a\")  s.append(5) }"));
}

TEST("TypeChecker rejects an unknown method on a String")
{
    EXPECT_THROWS(check("f() { s = String(\"a\")  s.push(\"b\") }"));
}

TEST("TypeChecker rejects indexing into a String - slicing is deliberately out of scope this "
     "phase (see docs/language/0042-string.md)")
{
    EXPECT_THROWS(check("f() -> i32 { s = String(\"a\")  return s[0] }"));
}

TEST("TypeChecker accepts a String argument where a str parameter is expected - 'String "
     "automatically lends a str' (see docs/std/strings/0001-str.md)")
{
    check("greet(name: str) -> str { return name } "
          "s = String(\"Axea\") "
          "x = greet(s)");
}

TEST("TypeChecker rejects a str argument where a String parameter is expected - lending only "
     "goes one direction")
{
    EXPECT_THROWS(check("useString(s: String) { called = s.append(\"x\") } "
                        "x = useString(\"not a String\")"));
}

TEST("TypeChecker accepts String as a parameter, return type, and local declared type")
{
    check("build() -> String { "
          "  x: String = String(\"a\") "
          "  return x "
          "} "
          "use(s: String) -> i32 { return s.length } "
          "n = build() "
          "y = use(n)");
}

TEST("TypeChecker rejects String as a struct field type")
{
    EXPECT_THROWS(check("struct Wrapper { text: String }"));
}

TEST("TypeChecker accepts Buffer() construction and append/append_line/clear/reserve/finish")
{
    check("f() -> String { "
          "  b = Buffer() "
          "  b.append(\"Axea\") "
          "  b.append_line(\" Language\") "
          "  b.clear() "
          "  b.reserve(8) "
          "  return b.finish() "
          "} "
          "x = f() "
          "n = x.length");
}

TEST("TypeChecker accepts Buffer .length and .capacity as i32 fields")
{
    check("f() -> i32 { "
          "  b = Buffer() "
          "  return b.length + b.capacity "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects Buffer() with any argument")
{
    EXPECT_THROWS(check("x = Buffer(\"a\")"));
}

TEST("TypeChecker rejects 'append'/'append_line' on a Buffer with a non-str-coercible argument")
{
    EXPECT_THROWS(check("f() { b = Buffer()  b.append(5) }"));
    EXPECT_THROWS(check("f() { b = Buffer()  b.append_line(true) }"));
}

TEST("TypeChecker rejects 'reserve' on a Buffer with a non-i32 argument")
{
    EXPECT_THROWS(check("f() { b = Buffer()  b.reserve(\"oops\") }"));
}

TEST("TypeChecker rejects 'finish' on a Buffer with any argument")
{
    EXPECT_THROWS(check("f() { b = Buffer()  b.finish(1) }"));
}

TEST("TypeChecker rejects an unknown method/field on a Buffer")
{
    EXPECT_THROWS(check("f() { b = Buffer()  b.push(\"x\") }"));
    EXPECT_THROWS(check("f() -> i32 { b = Buffer()  return b.count }"));
}

TEST("TypeChecker rejects indexing into a Buffer")
{
    EXPECT_THROWS(check("f() -> i32 { b = Buffer()  return b[0] }"));
}

TEST("TypeChecker distinguishes Buffer.append from String.append despite the shared method name")
{
    check("f() { "
          "  buf = Buffer() "
          "  buf.append(\"a\") "
          "  s = String(\"b\") "
          "  s.append(\"c\") "
          "}");
}

TEST("TypeChecker accepts Buffer as a parameter, return type, and local declared type")
{
    check("build() -> Buffer { "
          "  x: Buffer = Buffer() "
          "  return x "
          "} "
          "use(b: Buffer) -> i32 { return b.length } "
          "n = build() "
          "y = use(n)");
}

TEST("TypeChecker rejects Buffer as a struct field type")
{
    EXPECT_THROWS(check("struct Wrapper { text: Buffer }"));
}

TEST("TypeChecker accepts char literals and equality comparison")
{
    check("f() -> bool { "
          "  a = 'A' "
          "  b = 'B' "
          "  return a == b "
          "} "
          "x = f()");
}

TEST("TypeChecker accepts char ordering comparisons")
{
    check("f() -> bool { "
          "  a = 'A' "
          "  b = 'B' "
          "  lt = a < b "
          "  le = a <= b "
          "  gt = a > b "
          "  ge = a >= b "
          "  return lt "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects char arithmetic")
{
    EXPECT_THROWS(check("x = 'A' + 'B'"));
    EXPECT_THROWS(check("x = 'A' - 'B'"));
}

TEST("TypeChecker rejects comparing a char against an i32")
{
    EXPECT_THROWS(check("x = 'A' < 5"));
    EXPECT_THROWS(check("x = 5 == 'A'"));
}

TEST("TypeChecker rejects an empty or multi-character char literal")
{
    EXPECT_THROWS(check("x = ''"));
    EXPECT_THROWS(check("x = 'ab'"));
}

TEST("TypeChecker accepts char as a parameter, return type, local declared type, and struct "
     "field type")
{
    check("struct Letter { value: char } "
          "identity(c: char) -> char { return c } "
          "x: char = 'A' "
          "y = identity(x) "
          "l = Letter { value: 'Z' }");
}

TEST("TypeChecker accepts bounded, open-start, open-end, and fully-open str slice expressions")
{
    check("date = \"2026-08-18\" "
          "year = date[..4] "
          "month = date[5..7] "
          "day = date[8..] "
          "whole = date[..]");
}

TEST("TypeChecker accepts slicing a String - String lends a str the same way .append does")
{
    check("s = String(\"Axea\") "
          "x = s[0..2]");
}

TEST("TypeChecker requires i32 slice bounds")
{
    EXPECT_THROWS(check("date = \"2026-08-18\"  x = date[\"a\"..4]"));
    EXPECT_THROWS(check("date = \"2026-08-18\"  x = date[0..true]"));
}

TEST("TypeChecker rejects slicing a non-str-coercible type")
{
    EXPECT_THROWS(check("x = [1, 2, 3][..2]"));
    EXPECT_THROWS(check("x = 5[..2]"));
}

TEST("TypeChecker types a str slice expression as str, not String")
{
    check("date = \"2026-08-18\" "
          "greet(name: str) -> str { return name } "
          "x = greet(date[..4])");
}

TEST("TypeChecker accepts parse<i32>() and parse<bool>(), typing the result as i32/bool "
     "respectively")
{
    check("n: i32 = \"42\".parse<i32>() "
          "b: bool = \"true\".parse<bool>()");
}

TEST("TypeChecker accepts parse<T>() on a String, str-coerced the same way .append's own "
     "argument is")
{
    check("s = String(\"42\") "
          "n = s.parse<i32>()");
}

TEST("TypeChecker rejects parse<T>() on a non-str-coercible object")
{
    EXPECT_THROWS(check("x = 5.parse<i32>()"));
    EXPECT_THROWS(check("x = true.parse<i32>()"));
}

TEST("TypeChecker rejects parse<T>() for an unsupported target type")
{
    EXPECT_THROWS(check("x = \"5\".parse<str>()"));
    EXPECT_THROWS(check("x = \"5\".parse<char>()"));
}

TEST("TypeChecker rejects parse() with no explicit type argument")
{
    EXPECT_THROWS(check("x = \"5\".parse()"));
}

TEST("TypeChecker rejects parse<T>() called with an argument")
{
    EXPECT_THROWS(check("x = \"5\".parse<i32>(1)"));
}

TEST("TypeChecker still parses/checks 'field < expr' as a comparison, not a misfired generic "
     "call, when the field itself happens to be i32")
{
    check("struct P { field: i32 } "
          "f(p: P) -> bool { return p.field < 10 } "
          "p = P { field: 5 } "
          "x = f(p)");
}

TEST("TypeChecker accepts .length and .bytes on a bare str - previously str had no field "
     "access at all")
{
    check("f() -> i32 { "
          "  s = \"hello\" "
          "  return s.length + s.bytes "
          "} "
          "x = f()");
}

TEST("TypeChecker accepts .length and .bytes on String")
{
    check("f() -> i32 { "
          "  s = String(\"hello\") "
          "  return s.length + s.bytes "
          "} "
          "x = f()");
}

TEST("TypeChecker accepts .length, .bytes, and .capacity on Buffer")
{
    check("f() -> i32 { "
          "  b = Buffer() "
          "  return b.length + b.bytes + b.capacity "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects an unknown field on str, suggesting length/bytes")
{
    EXPECT_THROWS(check("x = \"hi\".foo"));
}
