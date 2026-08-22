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

TEST("RegionChecker rejects returning a struct value read via Map<K,V>.get() from a borrowed "
     "Map parameter (generic K/V - see docs/language/0034-maps-and-sets.md)")
{
    const std::string source = "struct Point { x: i32 } "
                               "leak(m: Map<i32,Point>) -> Point { return m.get(1) } "
                               "a = Map<i32,Point>() "
                               "b = a.set(1, Point { x: 1 }) "
                               "x = leak(a)";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker accepts a take Map<K,V> parameter's .get() result being returned")
{
    const std::string source = "struct Point { x: i32 } "
                               "consume(take m: Map<i32,Point>) -> Point { return m.get(1) } "
                               "a = Map<i32,Point>() "
                               "b = a.set(1, Point { x: 1 }) "
                               "x = consume(a)";
    checkRegions(source);
}

TEST("RegionChecker accepts a primitive value read via Map<K,V>.get() from a borrowed parameter")
{
    checkRegions("first(m: Map<i32,i32>) -> i32 { return m.get(1) } "
                 "a = Map<i32,i32>() "
                 "b = a.set(1, 1) "
                 "x = first(a)");
}

TEST("RegionChecker rejects returning a struct value read via SortedMap<K,V>.get() from a "
     "borrowed SortedMap parameter (see docs/language/0040-sorted-maps.md)")
{
    const std::string source = "struct Point { x: i32 } "
                               "leak(m: SortedMap<i32,Point>) -> Point { return m.get(1) } "
                               "a = SortedMap<i32,Point>() "
                               "b = a.set(1, Point { x: 1 }) "
                               "x = leak(a)";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker accepts a take SortedMap<K,V> parameter's .get() result being returned")
{
    const std::string source = "struct Point { x: i32 } "
                               "consume(take m: SortedMap<i32,Point>) -> Point { return m.get(1) } "
                               "a = SortedMap<i32,Point>() "
                               "b = a.set(1, Point { x: 1 }) "
                               "x = consume(a)";
    checkRegions(source);
}

TEST("RegionChecker accepts a primitive value read via SortedMap<K,V>.get() from a borrowed "
     "parameter")
{
    checkRegions("first(m: SortedMap<i32,i32>) -> i32 { return m.get(1) } "
                 "a = SortedMap<i32,i32>() "
                 "b = a.set(1, 1) "
                 "x = first(a)");
}

TEST("RegionChecker rejects returning a borrowed SortedMap<K,V> parameter directly")
{
    EXPECT_THROWS(checkRegions("leak(m: SortedMap<i32,i32>) -> SortedMap<i32,i32> { return m } "
                               "a = SortedMap<i32,i32>() "
                               "x = leak(a)"));
}

TEST("RegionChecker accepts a take SortedMap<K,V> parameter being returned directly")
{
    checkRegions("consume(take m: SortedMap<i32,i32>) -> SortedMap<i32,i32> { return m } "
                 "a = SortedMap<i32,i32>() "
                 "x = consume(a)");
}

TEST("RegionChecker rejects returning a borrowed SortedSet<T> parameter directly")
{
    EXPECT_THROWS(checkRegions("leak(s: SortedSet<i32>) -> SortedSet<i32> { return s } "
                               "a = SortedSet<i32>() "
                               "x = leak(a)"));
}

TEST("RegionChecker accepts a take SortedSet<T> parameter being returned directly")
{
    checkRegions("consume(take s: SortedSet<i32>) -> SortedSet<i32> { return s } "
                 "a = SortedSet<i32>() "
                 "x = consume(a)");
}

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

TEST("RegionChecker accepts an extern call through .to_cstr() on a borrowed str parameter")
{
    checkRegions("extern c puts(text: cstr) "
                 "greet(name: str) -> i32 { called = puts(name.to_cstr())  return 1 } "
                 "a = greet(\"hi\")");
}

TEST("RegionChecker rejects returning a struct value read via Stack<T>.peek() from a borrowed "
     "Stack parameter (see docs/language/0035-stacks.md)")
{
    const std::string source = "struct Point { x: i32 } "
                               "leak(s: Stack<Point>) -> Point { return s.peek() } "
                               "a = Stack<Point>() "
                               "b = a.push(Point { x: 1 }) "
                               "x = leak(a)";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker accepts a take Stack<T> parameter's .peek() result being returned")
{
    const std::string source = "struct Point { x: i32 } "
                               "consume(take s: Stack<Point>) -> Point { return s.peek() } "
                               "a = Stack<Point>() "
                               "b = a.push(Point { x: 1 }) "
                               "x = consume(a)";
    checkRegions(source);
}

TEST("RegionChecker accepts returning a struct value read via Stack<T>.pop() from a borrowed "
     "parameter (pop removes - unlike peek, nothing still aliases it)")
{
    const std::string source = "struct Point { x: i32 } "
                               "take_top(s: Stack<Point>) -> Point { return s.pop() } "
                               "a = Stack<Point>() "
                               "b = a.push(Point { x: 1 }) "
                               "x = take_top(a)";
    checkRegions(source);
}

TEST("RegionChecker accepts a primitive value read via Stack<T>.peek() from a borrowed parameter")
{
    checkRegions("first(s: Stack<i32>) -> i32 { return s.peek() } "
                 "a = Stack<i32>() "
                 "b = a.push(1) "
                 "x = first(a)");
}

TEST("RegionChecker rejects returning a borrowed LinkedList<T> parameter directly")
{
    EXPECT_THROWS(checkRegions("leak(s: LinkedList<i32>) -> LinkedList<i32> { return s } "
                               "a = LinkedList<i32>() "
                               "x = leak(a)"));
}

TEST("RegionChecker accepts a take LinkedList<T> parameter being returned directly")
{
    checkRegions("consume(take s: LinkedList<i32>) -> LinkedList<i32> { return s } "
                 "a = LinkedList<i32>() "
                 "x = consume(a)");
}

TEST("RegionChecker accepts returning a struct value read via LinkedList<T>.pop_front() from a "
     "borrowed parameter (pop_front removes - no peek_front exists, so no aliasing exception is "
     "needed at all - see docs/language/0036-linked-lists.md)")
{
    const std::string source = "struct Point { x: i32 } "
                               "take_front(s: LinkedList<Point>) -> Point { return s.pop_front() } "
                               "a = LinkedList<Point>() "
                               "b = a.push_front(Point { x: 1 }) "
                               "x = take_front(a)";
    checkRegions(source);
}

TEST("RegionChecker accepts returning a struct value read via LinkedList<T>.pop_back() from a "
     "borrowed parameter")
{
    const std::string source = "struct Point { x: i32 } "
                               "take_back(s: LinkedList<Point>) -> Point { return s.pop_back() } "
                               "a = LinkedList<Point>() "
                               "b = a.push_front(Point { x: 1 }) "
                               "x = take_back(a)";
    checkRegions(source);
}

TEST("RegionChecker accepts a primitive value read via LinkedList<T>.pop_front() from a borrowed "
     "parameter")
{
    checkRegions("first(s: LinkedList<i32>) -> i32 { return s.pop_front() } "
                 "a = LinkedList<i32>() "
                 "b = a.push_front(1) "
                 "x = first(a)");
}

TEST("RegionChecker rejects returning a borrowed Deque<T> parameter directly")
{
    EXPECT_THROWS(checkRegions("leak(d: Deque<i32>) -> Deque<i32> { return d } "
                               "a = Deque<i32>() "
                               "x = leak(a)"));
}

TEST("RegionChecker accepts a take Deque<T> parameter being returned directly")
{
    checkRegions("consume(take d: Deque<i32>) -> Deque<i32> { return d } "
                 "a = Deque<i32>() "
                 "x = consume(a)");
}

TEST("RegionChecker rejects returning a struct value read via Deque<T>[i] from a borrowed "
     "parameter - reuses IndexExpr's existing generic aliasing rule (see "
     "docs/language/0037-deques.md), the same rule array/List indexing already has, not a new "
     "MethodCallExpr exception")
{
    const std::string source = "struct Point { x: i32 } "
                               "leak(d: Deque<Point>) -> Point { return d[0] } "
                               "a = Deque<Point>() "
                               "b = a.push_back(Point { x: 1 }) "
                               "x = leak(a)";
    EXPECT_THROWS(checkRegions(source));
}

TEST("RegionChecker accepts a take Deque<T> parameter's [i] result being returned")
{
    const std::string source = "struct Point { x: i32 } "
                               "consume(take d: Deque<Point>) -> Point { return d[0] } "
                               "a = Deque<Point>() "
                               "b = a.push_back(Point { x: 1 }) "
                               "x = consume(a)";
    checkRegions(source);
}

TEST("RegionChecker accepts returning a struct value read via Deque<T>.pop_front() from a "
     "borrowed parameter (pop_front removes - unlike [i], nothing still aliases it)")
{
    const std::string source = "struct Point { x: i32 } "
                               "take_front(d: Deque<Point>) -> Point { return d.pop_front() } "
                               "a = Deque<Point>() "
                               "b = a.push_back(Point { x: 1 }) "
                               "x = take_front(a)";
    checkRegions(source);
}

TEST("RegionChecker accepts a primitive value read via Deque<T>[i] from a borrowed parameter")
{
    checkRegions("first(d: Deque<i32>) -> i32 { return d[0] } "
                 "a = Deque<i32>() "
                 "b = a.push_back(1) "
                 "x = first(a)");
}

TEST("RegionChecker rejects returning a borrowed Queue<T> parameter directly")
{
    EXPECT_THROWS(checkRegions("leak(q: Queue<i32>) -> Queue<i32> { return q } "
                               "a = Queue<i32>() "
                               "x = leak(a)"));
}

TEST("RegionChecker accepts a take Queue<T> parameter being returned directly")
{
    checkRegions("consume(take q: Queue<i32>) -> Queue<i32> { return q } "
                 "a = Queue<i32>() "
                 "x = consume(a)");
}

TEST("RegionChecker accepts returning a struct value read via Queue<T>.dequeue() from a "
     "borrowed parameter (dequeue removes - the simplest region-checking story of any "
     "collection this session, no peek and no indexing at all - see "
     "docs/language/0038-queues.md)")
{
    const std::string source = "struct Point { x: i32 } "
                               "take_first(q: Queue<Point>) -> Point { return q.dequeue() } "
                               "a = Queue<Point>() "
                               "b = a.enqueue(Point { x: 1 }) "
                               "x = take_first(a)";
    checkRegions(source);
}

TEST("RegionChecker accepts a primitive value read via Queue<T>.dequeue() from a borrowed "
     "parameter")
{
    checkRegions("first(q: Queue<i32>) -> i32 { return q.dequeue() } "
                 "a = Queue<i32>() "
                 "b = a.enqueue(1) "
                 "x = first(a)");
}

TEST("RegionChecker rejects returning a borrowed PriorityQueue<T> parameter directly")
{
    EXPECT_THROWS(checkRegions("leak(q: PriorityQueue<i32>) -> PriorityQueue<i32> { return q } "
                               "a = PriorityQueue<i32>() "
                               "x = leak(a)"));
}

TEST("RegionChecker accepts a take PriorityQueue<T> parameter being returned directly")
{
    checkRegions("consume(take q: PriorityQueue<i32>) -> PriorityQueue<i32> { return q } "
                 "a = PriorityQueue<i32>() "
                 "x = consume(a)");
}

TEST("RegionChecker accepts a primitive value read via PriorityQueue<T>.pop() from a borrowed "
     "parameter (T is i32-only this phase, so no struct-aliasing case is even reachable - see "
     "docs/language/0039-priority-queues.md)")
{
    checkRegions("first(q: PriorityQueue<i32>) -> i32 { return q.pop() } "
                 "a = PriorityQueue<i32>() "
                 "b = a.push(1) "
                 "x = first(a)");
}

TEST("RegionChecker accepts a primitive value read via PriorityQueue<T>.peek() from a borrowed "
     "parameter")
{
    checkRegions("top(q: PriorityQueue<i32>) -> i32 { return q.peek() } "
                 "a = PriorityQueue<i32>() "
                 "b = a.push(1) "
                 "x = top(a)");
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

TEST("RegionChecker treats a slice of a borrowed Array parameter as Owned - a slice always "
     "allocates a fresh List<T>, never aliasing the source (see "
     "docs/language/0050-collection-join-and-slicing.md)")
{
    checkRegions("firstTwo(nums: [i32; 4]) -> List<i32> { return nums[..2] } "
                 "x = firstTwo([1, 2, 3, 4])");
}

TEST("RegionChecker treats a .join() of a borrowed Array parameter as Owned - always allocates a "
     "fresh String, never aliasing the source")
{
    checkRegions("describe(nums: [i32; 4]) -> String { return nums.join(\",\") } "
                 "x = describe([1, 2, 3, 4])");
}
