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

TEST("TypeChecker accepts push/pop/peek/.length on a PriorityQueue<char> - char is orderable "
     "by codepoint, same as i32 (see docs/language/0044-char.md)")
{
    check("f() -> char { "
          "  q = PriorityQueue<char>() "
          "  q.push('B') "
          "  q.push('A') "
          "  n = q.length "
          "  x = q.peek() "
          "  y = q.pop() "
          "  return y "
          "} "
          "z = f()");
}

TEST("TypeChecker accepts push/pop/peek/.length on a PriorityQueue<str> - str has a real "
     "lexicographic order, same as i32/char (see docs/language/0042-string.md)")
{
    check("f() -> str { "
          "  q = PriorityQueue<str>() "
          "  q.push(\"banana\") "
          "  q.push(\"apple\") "
          "  n = q.length "
          "  x = q.peek() "
          "  y = q.pop() "
          "  return y "
          "} "
          "z = f()");
}

TEST("TypeChecker rejects the owned String type as a PriorityQueue<T> element - orderability, "
     "like Set<T>/Map<K,V>'s own hashability, only ever considers the bare str value type, not "
     "the owned String type it's otherwise str-coercible to")
{
    EXPECT_THROWS(check("q = PriorityQueue<String>()"));
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

TEST("TypeChecker rejects a non-orderable key type on SortedMap<K,V> - bool has no total order, "
     "and the owned String type isn't orderable even though it's str-coercible everywhere else "
     "(see docs/language/0040-sorted-maps.md)")
{
    EXPECT_THROWS(check("m = SortedMap<bool,i32>()"));
    EXPECT_THROWS(check("m = SortedMap<String,i32>()"));
}

TEST("TypeChecker accepts set/get/contains/remove/.length on a SortedMap<char,i32> - char is "
     "orderable by codepoint, same as i32 (see docs/language/0044-char.md)")
{
    check("f() -> i32 { "
          "  m = SortedMap<char,i32>() "
          "  m.set('A', 1) "
          "  hit: bool = m.contains('A') "
          "  v = m.get('A') "
          "  m.remove('A') "
          "  return v + m.length "
          "} "
          "x = f()");
}

TEST("TypeChecker accepts set/get/contains/remove/.length on a SortedMap<str,i32> - str has a "
     "real lexicographic order, same as i32/char (see docs/language/0042-string.md)")
{
    check("f() -> i32 { "
          "  m = SortedMap<str,i32>() "
          "  m.set(\"a\", 1) "
          "  hit: bool = m.contains(\"a\") "
          "  v = m.get(\"a\") "
          "  m.remove(\"a\") "
          "  return v + m.length "
          "} "
          "x = f()");
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

TEST("TypeChecker rejects a non-orderable element type on SortedSet<T> - bool has no total "
     "order, and the owned String type isn't orderable even though it's str-coercible "
     "everywhere else (see docs/language/0041-sorted-sets.md)")
{
    EXPECT_THROWS(check("s = SortedSet<bool>()"));
    EXPECT_THROWS(check("s = SortedSet<String>()"));
}

TEST("TypeChecker accepts add/contains/remove/.length on a SortedSet<str> - str has a real "
     "lexicographic order, same as i32/char (see docs/language/0042-string.md)")
{
    check("f() -> i32 { "
          "  s = SortedSet<str>() "
          "  s.add(\"a\") "
          "  s.add(\"b\") "
          "  before = s.contains(\"b\") "
          "  s.remove(\"b\") "
          "  return s.length "
          "} "
          "x = f()");
}

TEST("TypeChecker accepts add/contains/remove/.length on a SortedSet<char> - char is orderable "
     "by codepoint, same as i32 (see docs/language/0044-char.md)")
{
    check("f() -> i32 { "
          "  s = SortedSet<char>() "
          "  s.add('A') "
          "  s.add('B') "
          "  before = s.contains('B') "
          "  s.remove('B') "
          "  return s.length "
          "} "
          "x = f()");
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

TEST("TypeChecker accepts Buffer.write as a str-coercible-argument alias of append (see "
     "docs/language/0061-buffer-write.md)")
{
    check("f() -> String { "
          "  b = Buffer() "
          "  b.write(\"Axea\") "
          "  return b.finish() "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects Buffer.write with a non-str-coercible argument")
{
    EXPECT_THROWS(check("f() { b = Buffer()  b.write(5) }"));
}

TEST("TypeChecker rejects Buffer.write with the wrong argument count")
{
    EXPECT_THROWS(check("f() { b = Buffer()  b.write() }"));
    EXPECT_THROWS(check("f() { b = Buffer()  b.write(\"a\", \"b\") }"));
}

TEST("TypeChecker rejects 'write' on a String - only Buffer has it")
{
    EXPECT_THROWS(check("f() { s = String(\"a\")  s.write(\"b\") }"));
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

TEST("TypeChecker accepts a well-formed impl Display for a struct, typechecking format's own "
     "body with self bound to the struct type (see docs/language/0062-display-trait.md)")
{
    check("struct Point { x: i32  y: i32 } "
          "trait Display { format(self, buf: Buffer) } "
          "impl Display for Point { "
          "  format(self, buf: Buffer) { buf.write(\"({self.x}, {self.y})\") } "
          "}");
}

TEST("TypeChecker rejects impl Display for a struct with no 'format' method at all")
{
    EXPECT_THROWS(check("struct Point { x: i32 } "
                        "impl Display for Point { render(self, buf: Buffer) { } }"));
}

TEST("TypeChecker rejects impl Display for a struct whose 'format' has the wrong parameter count")
{
    EXPECT_THROWS(check("struct Point { x: i32 } "
                        "impl Display for Point { format(self) { } }"));
}

TEST("TypeChecker rejects impl for an unknown (non-struct) type")
{
    EXPECT_THROWS(check("impl Display for Ghost { format(self, buf: Buffer) { } }"));
}

TEST("TypeChecker rejects an impl missing a method its own matching trait declares")
{
    EXPECT_THROWS(check("struct Point { x: i32 } "
                        "trait Display { format(self, buf: Buffer)  extra(self) } "
                        "impl Display for Point { format(self, buf: Buffer) { } }"));
}

TEST("TypeChecker rejects an impl method whose arity disagrees with its matching trait's "
     "declared signature")
{
    EXPECT_THROWS(check("struct Point { x: i32 } "
                        "trait Display { format(self, buf: Buffer) } "
                        "impl Display for Point { format(self) { } }"));
}

TEST("TypeChecker accepts a real field access on self inside an impl method body")
{
    check("struct Point { x: i32  y: i32 } "
          "impl Display for Point { "
          "  format(self, buf: Buffer) -> i32 { return self.x + self.y } "
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

TEST("TypeChecker accepts str ordering comparisons")
{
    check("f() -> bool { "
          "  a = \"apple\" "
          "  b = \"banana\" "
          "  lt = a < b "
          "  le = a <= b "
          "  gt = a > b "
          "  ge = a >= b "
          "  return lt "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects ordering comparisons on the owned String type - orderability only "
     "ever considers the bare str value type, even though String is str-coercible everywhere "
     "else in this language (see docs/language/0042-string.md)")
{
    EXPECT_THROWS(check("f() { a = String(\"a\")  b = String(\"b\")  x = a < b }"));
}

TEST("TypeChecker accepts i64 arithmetic and comparisons, typing the result i64/bool "
     "respectively (see docs/language/0005-type-system.md)")
{
    check("f() -> i64 { "
          "  a = 100i64 "
          "  b = 25i64 "
          "  sum = a + b "
          "  diff = a - b "
          "  prod = a * b "
          "  quot = a / b "
          "  lt = a < b "
          "  return sum + diff + prod + quot "
          "} "
          "x = f()");
}

TEST("TypeChecker accepts f64 arithmetic and comparisons, typing the result f64/bool "
     "respectively")
{
    check("f() -> f64 { "
          "  a = 1.5 "
          "  b = 2.5 "
          "  sum = a + b "
          "  quot = a / b "
          "  lt = a < b "
          "  return sum + quot "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects mixing i32/i64/f64 in one arithmetic or comparison expression - no "
     "implicit widening, matching every other mixed-type binary op in this checker")
{
    EXPECT_THROWS(check("x = 1 + 100i64"));
    EXPECT_THROWS(check("x = 100i64 + 1.5"));
    EXPECT_THROWS(check("x = 1 < 1.5"));
}

TEST("TypeChecker accepts an 'as' cast between any two of i32/i64/f64, including a same-kind "
     "cast, typing the result as targetType")
{
    check("f() -> i64 { "
          "  a = 5 "
          "  b = a as i64 "
          "  c = b as f64 "
          "  d = c as i32 "
          "  e = a as i32 "
          "  return b "
          "} "
          "x = f()");
}

TEST("TypeChecker rejects an 'as' cast to/from a non-numeric type")
{
    EXPECT_THROWS(check("x = true as i64"));
    EXPECT_THROWS(check("x = 5 as bool"));
    EXPECT_THROWS(check("x = \"a\" as i32"));
    EXPECT_THROWS(check("x = 5 as str"));
}

TEST("TypeChecker accepts i64/f64 as print/write/interpolation arguments")
{
    check("a = 100i64 "
          "b = 1.5 "
          "p1 = print(a) "
          "p2 = print(b) "
          "s = \"n={a} f={b}\"");
}

TEST("TypeChecker accepts push/pop/peek/.length on a PriorityQueue<i64> and PriorityQueue<f64> "
     "- both are orderable, same as i32/char/str (see docs/language/0039-priority-queues.md)")
{
    check("f() { "
          "  qi = PriorityQueue<i64>() "
          "  qi.push(100i64) "
          "  y = qi.pop() "
          "  qf = PriorityQueue<f64>() "
          "  qf.push(1.5) "
          "  z = qf.pop() "
          "}");
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

TEST("TypeChecker rejects slicing a non-sliceable type")
{
    // Array/List slicing of i32/bool/char/str/String elements is now
    // supported (see docs/language/0050-collection-join-and-slicing.md) -
    // a bare i32 is still not sliceable at all.
    EXPECT_THROWS(check("x = 5[..2]"));
}

TEST("TypeChecker types a str slice expression as str, not String")
{
    check("date = \"2026-08-18\" "
          "greet(name: str) -> str { return name } "
          "x = greet(date[..4])");
}

TEST("TypeChecker accepts single-character indexing on str and the owned String type, "
     "typing the result char (see docs/language/0047-unicode.md)")
{
    check("s = \"hello\" "
          "c: char = s[0]");
    check("s = String(\"hello\") "
          "c: char = s[0]");
}

TEST("TypeChecker rejects a non-i32 index into a str/String")
{
    EXPECT_THROWS(check("s = \"hello\"  x = s[true]"));
    EXPECT_THROWS(check("s = \"hello\"  x = s['a']"));
}

TEST("TypeChecker still rejects indexed assignment into a str - single-character indexing is "
     "read-only, str stays immutable (isIndexable itself is untouched)")
{
    EXPECT_THROWS(check("f() { s = \"hello\"  s[0] = 'x' }"));
}

TEST("TypeChecker accepts enum variant construction (both payload and bare no-payload forms) "
     "and a real match with full exhaustiveness (see docs/language/0064-enums.md)")
{
    check("enum Shape { Circle(f64)  Rectangle(f64, f64)  Point } "
          "area(s: Shape) -> f64 { "
          "  return match s { "
          "    Circle(r) => 3.14159 * r * r "
          "    Rectangle(w, h) => w * h "
          "    Point => 0.0 "
          "  } "
          "} "
          "c = Shape.Circle(5.0) "
          "p = Shape.Point "
          "x = area(c)");
}

TEST("TypeChecker accepts a wildcard arm covering the remaining variants")
{
    check("enum Shape { Circle(f64)  Rectangle(f64, f64)  Point } "
          "f(s: Shape) -> str { return match s { Circle(r) => \"c\"  _ => \"other\" } } "
          "x = f(Shape.Point)");
}

TEST("TypeChecker rejects a non-exhaustive match with no wildcard arm")
{
    EXPECT_THROWS(check("enum Shape { Circle(f64)  Point } "
                        "f(s: Shape) -> f64 { return match s { Circle(r) => r } } "
                        "x = f(Shape.Point)"));
}

TEST("TypeChecker rejects a wildcard arm that isn't the last arm")
{
    EXPECT_THROWS(check("enum Shape { Circle(f64)  Point } "
                        "f(s: Shape) -> str { return match s { _ => \"w\"  Circle(r) => \"c\" } } "
                        "x = f(Shape.Point)"));
}

TEST("TypeChecker rejects a variant matched more than once in the same match expression")
{
    EXPECT_THROWS(
        check("enum Shape { Circle(f64)  Point } "
              "f(s: Shape) -> str { "
              "  return match s { Circle(r) => \"a\"  Circle(x) => \"b\"  Point => \"p\" } "
              "} "
              "x = f(Shape.Point)"));
}

TEST("TypeChecker rejects a match arm whose binding count doesn't match its variant's own "
     "payload arity")
{
    EXPECT_THROWS(check("enum Shape { Circle(f64)  Point } "
                        "f(s: Shape) -> str { "
                        "  return match s { Circle(r, extra) => \"c\"  Point => \"p\" } "
                        "} "
                        "x = f(Shape.Point)"));
}

TEST("TypeChecker rejects match arms with incompatible result types")
{
    EXPECT_THROWS(check("enum Shape { Circle(f64)  Point } "
                        "f(s: Shape) { "
                        "  return match s { Circle(r) => 1  Point => \"p\" } "
                        "} "
                        "x = 1"));
}

TEST("TypeChecker rejects 'match' on a non-enum value")
{
    EXPECT_THROWS(check("f() { return match 5 { Circle(r) => r } } x = 1"));
}

TEST("TypeChecker rejects a variant construction with the wrong argument count or type")
{
    EXPECT_THROWS(check("enum Shape { Circle(f64) } x = Shape.Circle(5.0, 6.0)"));
    EXPECT_THROWS(check("enum Shape { Circle(f64) } x = Shape.Circle(\"wrong\")"));
    EXPECT_THROWS(check("enum Shape { Circle(f64) } x = Shape.Point"));
}

TEST("TypeChecker rejects a no-payload variant referenced with an explicit argument, and a "
     "payload variant referenced with no arguments via the bare-field form")
{
    EXPECT_THROWS(check("enum Shape { Point } x = Shape.Point(5)"));
    EXPECT_THROWS(check("enum Shape { Circle(f64) } x = Shape.Circle"));
}

TEST("TypeChecker resolves a nested enum type used as another enum's own variant payload")
{
    check("enum Inner { A  B } "
          "enum Outer { Wrap(Inner) } "
          "x = Outer.Wrap(Inner.A)");
}

TEST("TypeChecker accepts Ok(x)/Err(e) against a declared Result<T,E> type, and typechecks "
     "'?' propagation through a Result<T,E>-returning function (see docs/language/0063-result.md)")
{
    check("divide(a: i32, b: i32) -> Result<i32, str> { "
          "  if b == 0 { return Err(\"division by zero\") } "
          "  return Ok(a / b) "
          "} "
          "x: Result<i32, str> = divide(10, 2)");
}

TEST("TypeChecker propagates '?' through a Result<T,E>-returning function, unwrapping Ok and "
     "requiring the operand's own Err type to match the enclosing function's")
{
    check("inner(a: i32) -> Result<i32, str> { return Ok(a) } "
          "outer(a: i32) -> Result<i32, str> { x = inner(a)?  return Ok(x) } "
          "y = outer(1)");
}

TEST("TypeChecker rejects '?' when the operand's Err type doesn't match the enclosing "
     "function's own Err type - no automatic error-type conversion this phase")
{
    EXPECT_THROWS(check("inner(a: i32) -> Result<i32, str> { return Ok(a) } "
                        "outer(a: i32) -> Result<i32, i32> { x = inner(a)?  return Ok(x) } "
                        "y = outer(1)"));
}

TEST("TypeChecker rejects '?' used inside a function whose own return type is neither "
     "Optional<T> nor Result<T,E>")
{
    EXPECT_THROWS(check("f(a: i32) -> Result<i32, str> { return Ok(a) } "
                        "g(a: i32) -> i32 { return f(a)? } "
                        "y = g(1)"));
}

TEST("TypeChecker rejects a bare Ok(...)/Err(...) with no declared Result<T,E> context")
{
    EXPECT_THROWS(check("x = Ok(5)"));
    EXPECT_THROWS(check("x = Err(5)"));
}

TEST("TypeChecker rejects Ok(...)/Err(...) whose value doesn't match the declared type's own "
     "Ok/Err type")
{
    EXPECT_THROWS(check("x: Result<i32, str> = Ok(\"wrong\")"));
    EXPECT_THROWS(check("x: Result<i32, str> = Err(5)"));
}

TEST("TypeChecker's unwrap_or/is_ok/is_err accept a Result<T,E>, unwrap_or's default must "
     "match the Ok type")
{
    check("f() -> Result<i32, str> { return Ok(5) } "
          "r = f() "
          "v = r.unwrap_or(0) "
          "ok = r.is_ok() "
          "err = r.is_err()");
    EXPECT_THROWS(check("f() -> Result<i32, str> { return Ok(5) } "
                        "r = f() "
                        "v = r.unwrap_or(\"wrong\")"));
}

TEST("TypeChecker rejects is_ok/is_err on an Optional<T>, and unwrap_or/is_some/is_none on a "
     "Result<T,E> - the two APIs stay distinct by name despite sharing unwrap_or")
{
    EXPECT_THROWS(check("f() -> Optional<i32> { return Some(5) } "
                        "o = f() "
                        "x = o.is_ok()"));
    EXPECT_THROWS(check("f() -> Result<i32, str> { return Ok(5) } "
                        "r = f() "
                        "x = r.is_some()"));
}

TEST("TypeChecker resolves a nested Result<T,E> type - E itself a Result, and T itself a "
     "Map<K,V> - via bracket-depth-aware comma splitting")
{
    check("x: Result<i32, Result<i32, str>> = Ok(1) "
          "y: Result<Map<i32,i32>, str> = Err(\"e\")");
}

TEST("TypeChecker accepts parse<i32>() and parse<bool>(), typing the result as "
     "Optional<i32>/Optional<bool> respectively (see docs/language/0052-optional.md)")
{
    check("n: Optional<i32> = \"42\".parse<i32>() "
          "b: Optional<bool> = \"true\".parse<bool>()");
}

TEST("TypeChecker accepts parse<i64>() and parse<f64>(), typing the result as "
     "Optional<i64>/Optional<f64> respectively (see docs/language/0051-numeric-widening.md "
     "and docs/language/0052-optional.md)")
{
    check("n: Optional<i64> = \"123456789012\".parse<i64>() "
          "f: Optional<f64> = \"3.14\".parse<f64>()");
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

TEST("TypeChecker accepts an extern c declaration and a call to it")
{
    check("extern c puts(text: cstr) "
          "s = \"hi\" "
          "c = s.to_cstr() "
          "called = puts(c)");
}

TEST("TypeChecker types .to_cstr() as cstr, distinct from str - no implicit coercion either way")
{
    EXPECT_THROWS(check("extern c puts(text: cstr) "
                        "s = \"hi\" "
                        "called = puts(s)")); // str, not cstr - rejected
}

TEST("TypeChecker accepts .to_cstr() on a String, str-coerced the same way .append's own "
     "argument is")
{
    check("s = String(\"hi\") "
          "x = s.to_cstr()");
}

TEST("TypeChecker rejects .to_cstr() on a non-str-coercible type")
{
    EXPECT_THROWS(check("x = 5.to_cstr()"));
}

TEST("TypeChecker rejects an extern parameter type that isn't FFI-safe")
{
    EXPECT_THROWS(check("extern c foo(x: char)"));
    EXPECT_THROWS(check("extern c foo(x: String)"));
    EXPECT_THROWS(check("extern c foo(x: List<i32>)"));
}

TEST("TypeChecker rejects an extern return type that isn't FFI-safe")
{
    EXPECT_THROWS(check("extern c foo() -> String"));
    EXPECT_THROWS(check("extern c foo() -> char"));
}

TEST("TypeChecker accepts every FFI-safe extern parameter/return type: i32, bool, str, cstr")
{
    check("extern c f1(x: i32) -> i32 "
          "extern c f2(x: bool) -> bool "
          "extern c f3(x: str) -> str "
          "extern c f4(x: cstr) -> cstr "
          "extern c f5(x: i32)"); // omitted return type => unit
}

TEST("TypeChecker rejects an extern function that has the same name as a real Axea function")
{
    EXPECT_THROWS(check("extern c foo(x: i32) "
                        "foo(x: i32) -> i32 { return x } "
                        "y = foo(1)"));
    EXPECT_THROWS(check("foo(x: i32) -> i32 { return x } "
                        "extern c foo(x: i32) "
                        "y = foo(1)"));
}

TEST("TypeChecker rejects an unsupported extern calling convention")
{
    EXPECT_THROWS(check("extern rust foo(x: i32)"));
}

TEST("TypeChecker rejects calling an undefined function/extern")
{
    EXPECT_THROWS(check("x = undefinedThing(1)"));
}

TEST("TypeChecker accepts print/write with i32, bool, char, str, and String arguments")
{
    check("run() -> i32 { print(\"hello\", 1, true, 'c') return 0 } r = run()");
    check("run() -> i32 { s = String(\"hi\") write(s) return 0 } r = run()");
    check("run() -> i32 { print() return 0 } r = run()");
}

TEST("TypeChecker accepts an Array/List argument to print(...)/write(...) - stringified via "
     "registerCollectionToStrRuntime (see docs/language/0054-collection-printing.md); "
     "slice<T> remains the one unsupported type")
{
    check("run() -> i32 { arr = [1, 2, 3] print(arr) return 0 } r = run()");
}

TEST("TypeChecker accepts print/write with a slice<T> argument (see "
     "docs/language/0056-slice-printing.md)")
{
    check("f(s: slice<i32>) -> i32 { print(s) return 0 } "
          "arr = [1, 2, 3] r = f(arr)");
}

TEST("TypeChecker rejects redefining 'print' or 'write' as a real function")
{
    EXPECT_THROWS(check("print(x: i32) -> i32 { return x }"));
    EXPECT_THROWS(check("extern c print(x: i32)"));
}

TEST("TypeChecker accepts a struct argument to print(...)/write(...) - it prints directly via "
     "the existing per-struct-type helper, no stringification needed (see "
     "docs/language/0049-printing-formatting.md's own follow-up)")
{
    check("struct Point { x: i32  y: i32 } "
          "p = Point { x: 1, y: 2 } "
          "print(\"point:\", p) "
          "write(p)");
}

TEST("TypeChecker accepts every collection kind as a print(...)/write(...) argument")
{
    check("m: Map<i32,i32> = Map<i32,i32>() print(m)");
    check("s: Set<i32> = Set<i32>() print(s)");
    check("dq: Deque<i32> = Deque<i32>() print(dq)");
    check("pq: PriorityQueue<i32> = PriorityQueue<i32>() print(pq)");
}

TEST("TypeChecker checks a bare top-level print(...)/write(...) call via the new ExprStmt "
     "case in TypeChecker::check's own top-level item loop (see "
     "docs/language/0049-printing-formatting.md's own Parsing follow-up)")
{
    check("print(\"hello\", 1, true)");
    check("write(\"loading\")");
}

TEST("TypeChecker types an interpolated string literal as String, matching the InterpolatedString"
     "Expr's own always-owned design")
{
    check("run() -> i32 { name = \"Ada\" s = \"hi {name}\" t = s.length return 0 } r = run()");
}

TEST("TypeChecker accepts an Array/List/slice<T> value inside an interpolation span (see "
     "docs/language/0054-collection-printing.md and docs/language/0056-slice-printing.md)")
{
    check("run() -> i32 { arr = [1, 2, 3] s = \"arr is {arr}\" return 0 } r = run()");
    check("f(sl: slice<i32>) -> i32 { s = \"sl is {sl}\" return 0 } "
          "arr = [1, 2, 3] r = f(arr)");
}

TEST("TypeChecker accepts i32/bool/char/str/String interpolation spans")
{
    check("run() -> i32 { "
          "n = 1 b = true c = 'x' s = \"hi\" "
          "out = \"{n} {b} {c} {s}\" "
          "return 0 } r = run()");
}

TEST("TypeChecker accepts slicing a fixed-size array of i32 into a List<i32> - the result "
     "supports indexing and .length like any other List")
{
    check("run() -> i32 { "
          "numbers = [1, 2, 3, 4] "
          "sliced = numbers[..2] "
          "first = sliced[0] "
          "len = sliced.length "
          "return first + len } r = run()");
}

TEST("TypeChecker accepts slicing a List<T> into another List<T>")
{
    check("run() -> i32 { "
          "numbers = List<i32>() "
          "pushed = numbers.push(1) "
          "sliced = numbers[..] "
          "first = sliced[0] "
          "return first } r = run()");
}

TEST("TypeChecker accepts slicing an Array/List of struct elements - slicing itself is a "
     "generic value copy, unrelated to text-representability (docs/language/0050-collection-"
     "join-and-slicing.md's own arrslice.copy loop shape); print(...)'s own wider allowlist "
     "(docs/language/0054-collection-printing.md) unblocked this as a side effect")
{
    check("struct Point { x: i32 } "
          "run() -> i32 { pts = [Point{x:1}] sliced = pts[..1] return 0 } "
          "r = run()");
}

TEST("TypeChecker accepts .join(separator) on an Array of i32, returning a String")
{
    check("run() -> i32 { "
          "numbers = [1, 2, 3] "
          "joined = numbers.join(\",\") "
          "len = joined.length "
          "return len } r = run()");
}

TEST("TypeChecker accepts .join(separator) on a List<str>")
{
    check("run() -> i32 { "
          "names = List<str>() "
          "pushed = names.push(\"ada\") "
          "joined = names.join(\", \") "
          "return joined.length } r = run()");
}

TEST("TypeChecker rejects .join on a non-Array/List type")
{
    EXPECT_THROWS(check("run() -> i32 { joined = (5).join(\",\") return 0 } r = run()"));
}

TEST("TypeChecker accepts .join on struct elements - each stringified via "
     "@axea.tostring.<Name> (see docs/language/0054-collection-printing.md)")
{
    check("struct Point { x: i32 } "
          "run() -> i32 { pts = [Point{x:1}] j = pts.join(\",\") return 0 } "
          "r = run()");
}

TEST("TypeChecker rejects .join with a non-str separator")
{
    EXPECT_THROWS(
        check("run() -> i32 { numbers = [1, 2, 3] joined = numbers.join(5) return 0 } r = run()"));
}

TEST("TypeChecker rejects .join with the wrong argument count")
{
    EXPECT_THROWS(
        check("run() -> i32 { numbers = [1, 2, 3] joined = numbers.join() return 0 } r = run()"));
}

TEST("TypeChecker accepts .join on a slice<T> receiver, same as Array/List (see "
     "docs/language/0056-slice-printing.md)")
{
    check("f(s: slice<i32>) -> String { return s.join(\",\") } "
          "arr = [1, 2, 3] r = f(arr)");
}

TEST("TypeChecker accepts numeric format specs on i32/i64/f64 interpolation spans (see "
     "docs/language/0055-numeric-format-specs.md)")
{
    check("run() -> i32 { n = 42 out = \"{n:05}\" return 0 } r = run()");
    check("run() -> i32 { n: i64 = 42i64 out = \"{n:x}\" return 0 } r = run()");
    check("run() -> i32 { pi = 3.14159 out = \"{pi:.2}\" return 0 } r = run()");
    check("run() -> i32 { n = 42 out = \"{n:X} {n:b} {n:o}\" return 0 } r = run()");
}

TEST("TypeChecker rejects a radix format spec (x/X/b/o) on a non-integer interpolation span")
{
    EXPECT_THROWS(check("run() -> i32 { pi = 3.14 out = \"{pi:x}\" return 0 } r = run()"));
    EXPECT_THROWS(check("run() -> i32 { s = \"hi\" out = \"{s:b}\" return 0 } r = run()"));
}

TEST("TypeChecker rejects a precision format spec ('.N') on a non-float interpolation span")
{
    EXPECT_THROWS(check("run() -> i32 { n = 42 out = \"{n:.2}\" return 0 } r = run()"));
}

TEST("TypeChecker rejects a plain width format spec (no type char) on a non-integer "
     "interpolation span")
{
    EXPECT_THROWS(check("run() -> i32 { pi = 3.14 out = \"{pi:05}\" return 0 } r = run()"));
}

TEST("TypeChecker rejects combining a radix type char with a precision in one format spec")
{
    EXPECT_THROWS(check("run() -> i32 { n = 42 out = \"{n:.2x}\" return 0 } r = run()"));
}

TEST("TypeChecker accepts an alignment format spec ('<'/'>'/'^' + width) on any "
     "isTextRepresentable type, not just i32/i64 - unlike bare width, which stays "
     "numeric-only (see docs/language/0057-alignment.md)")
{
    check("run() -> i32 { name = \"Ada\" out = \"{name:<20}\" return 0 } r = run()");
    check("run() -> i32 { ok = true out = \"{ok:^10}\" return 0 } r = run()");
    check("run() -> i32 { n = 42 out = \"{n:>10}\" return 0 } r = run()");
}

TEST("TypeChecker accepts an alignment format spec combined with a precision on an f64 "
     "interpolation span, matching the source doc's own {user.score:>8.2} example")
{
    check("run() -> i32 { pi = 3.14159 out = \"{pi:>8.2}\" return 0 } r = run()");
}

TEST("TypeChecker still rejects a radix conversion on a non-integer even when an alignment "
     "char is also present - alignment doesn't relax the radix/precision type restrictions")
{
    EXPECT_THROWS(check("run() -> i32 { pi = 3.14 out = \"{pi:>10x}\" return 0 } r = run()"));
    EXPECT_THROWS(check("run() -> i32 { n = 42 out = \"{n:>10.2}\" return 0 } r = run()"));
}

TEST("TypeChecker rejects an alignment char with no width to align within")
{
    EXPECT_THROWS(check("run() -> i32 { n = 42 out = \"{n:<}\" return 0 } r = run()"));
}

TEST("TypeChecker rejects combining zero-padding with an explicit alignment char - the two "
     "are mutually exclusive fill strategies")
{
    EXPECT_THROWS(check("run() -> i32 { n = 42 out = \"{n:<010}\" return 0 } r = run()"));
}

TEST("TypeChecker accepts self-doc '{expr=}' and debug '{expr:?}' on any isTextRepresentable "
     "type, including struct/collection - neither narrows the allowed type set (see "
     "docs/language/0058-debug-formatting.md)")
{
    check("run() -> i32 { n = 42 out = \"{n=}\" return 0 } r = run()");
    check("run() -> i32 { name = \"Ada\" out = \"{name=}\" return 0 } r = run()");
    check("struct Point { x: i32 } "
          "run() -> i32 { p = Point{x:1} out = \"{p:?}\" return 0 } r = run()");
    check("run() -> i32 { arr = [1, 2] out = \"{arr:?}\" return 0 } r = run()");
}

TEST("TypeChecker accepts a self-doc prefix combined with a numeric format spec, matching the "
     "source doc's own Python-style expression-debugging framing extended with a spec")
{
    check("run() -> i32 { pi = 3.14 out = \"{pi=:.2}\" return 0 } r = run()");
}

TEST("TypeChecker implicitly wraps a plain value into a union-typed call argument, declared "
     "local, and return, with no wrapper syntax (see docs/language/0065-unions.md)")
{
    check("f(x: i32 | str) -> i32 | str { return x } "
          "run() -> i32 { "
          "  y = f(5) "
          "  z = f(\"hi\") "
          "  w: i32 | str = 5 "
          "  return 0 "
          "} "
          "r = run()");
}

TEST("TypeChecker canonicalizes a union's alternatives - order doesn't affect its identity, so "
     "'str | i32' is assignable wherever 'i32 | str' is expected")
{
    check("f(x: i32 | str) -> i32 { return 0 } "
          "g(x: str | i32) -> i32 { return f(x) } "
          "y = g(5)");
}

TEST("TypeChecker resolves a union's own match arms by each alternative's own canonical type "
     "name, with full exhaustiveness checking exactly like a real enum")
{
    check("f(x: i32 | str) -> str { "
          "  return match x { i32(n) => \"number\"  str(s) => \"string\" } "
          "} "
          "y = f(5)");
}

TEST("TypeChecker rejects a non-exhaustive match on a union with no wildcard arm")
{
    EXPECT_THROWS(check("f(x: i32 | str) -> str { return match x { i32(n) => \"n\" } } y = f(5)"));
}

TEST("TypeChecker rejects a value whose type isn't any alternative of the declared union")
{
    EXPECT_THROWS(check("x: i32 | str = true"));
}

TEST("TypeChecker rejects a compound type (List<T>) as a union alternative - its own canonical "
     "name can't be spelled as a single match-arm-pattern identifier")
{
    EXPECT_THROWS(check("f(x: List<i32> | i32) -> i32 { return 0 } y = f(5)"));
}

TEST("TypeChecker accepts a struct as a union alternative, matched by its own type name")
{
    check("struct Point { x: i32  y: i32 } "
          "f(v: Point | i32) -> str { "
          "  return match v { Point(p) => \"point\"  i32(n) => \"number\" } "
          "} "
          "p = Point{x: 1, y: 2} "
          "a = f(p) "
          "b = f(5)");
}
