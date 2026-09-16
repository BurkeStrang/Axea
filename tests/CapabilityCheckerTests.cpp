#include "TestFramework.hpp"

#include "generics/GenericMonomorphizer.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/CapabilityChecker.hpp"
#include "sema/TypeChecker.hpp"

namespace
{
    // TypeChecker runs first in the real pipeline (compiler/main.cpp), so
    // exercise the same order here rather than feeding CapabilityChecker
    // programs that wouldn't actually type-check.
    std::unordered_map<std::string, std::vector<Capability>>
    capabilitiesOf(const std::string& source)
    {
        Lexer lexer(source);
        Parser parser(lexer.lex());
        auto program = parser.parseProgram();
        monomorphizeGenerics(program);

        TypeChecker typeChecker;
        typeChecker.check(program);

        CapabilityChecker capabilityChecker;
        capabilityChecker.check(program);
        return capabilityChecker.effectiveCapabilities();
    }
} // namespace

TEST("CapabilityChecker infers read for a parameter that is only read")
{
    const auto capabilities = capabilitiesOf(R"AXEA(struct Point
{
    i32 x
} i32 get(Point p)
{ return p.x } p = Point { x: 1 } x = get(p))AXEA");
    EXPECT_TRUE(capabilities.at("get")[0] == Capability::Read);
}

TEST("CapabilityChecker infers 'self' as read and 'buf' as write for an impl Display method - "
     "neither has an explicit capability prefix in source (see "
     "docs/language/0062-display-trait.md)")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Point
{
    i32 x
    i32 y
} impl Display for Point {   format(self, buf: Buffer) { buf.write("({self.x}, {self.y})") } })AXEA");
    const auto& formatCaps = capabilities.at("Point.format");
    EXPECT_TRUE(formatCaps[0] == Capability::Read);
    EXPECT_TRUE(formatCaps[1] == Capability::Write);
}

TEST("CapabilityChecker handles enum variant construction and 'match' with no false positives "
     "- a bare enum type name in EnumName.Variant(...)/EnumName.Variant position is never "
     "mistaken for a real parameter reference (see docs/language/0064-enums.md)")
{
    const auto capabilities = capabilitiesOf("enum Shape { Circle(f64)  Point } "
                                             "area(s: Shape) -> f64 { "
                                             "  return match s { Circle(r) => r  Point => 0.0 } "
                                             "} "
                                             "c = Shape.Circle(5.0) "
                                             "p = Shape.Point "
                                             "x = area(c)");
    EXPECT_TRUE(capabilities.at("area")[0] == Capability::Read);
}

TEST("CapabilityChecker handles a union-typed parameter and 'match' on it with no false "
     "positives - needs no special-casing at all (unlike a real enum's own EnumName.Variant "
     "construction syntax), since a union value is only ever produced by implicit wrapping, "
     "never a bare-type-name method/field access (see docs/language/0065-unions.md)")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(str f(i32 | str x)
{   return match x { i32(n) => "number"  str(s) => "string" } } y = f(5))AXEA");
    EXPECT_TRUE(capabilities.at("f")[0] == Capability::Read);
}

TEST("CapabilityChecker recurses into Ok(value)/Err(value)/'?' the same way it already does "
     "for Some(value)/None (see docs/language/0063-result.md) - a moved-from name wrapped in "
     "Ok(...) is still correctly tracked")
{
    const auto capabilities = capabilitiesOf(R"AXEA(Result<i32, i32> divide(i32 a, i32 b)
{   if b == 0 { return Err(a) }   return Ok(a / b) } x = divide(10, 2))AXEA");
    EXPECT_TRUE(capabilities.at("divide")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a parameter whose field is assigned")
{
    const auto capabilities = capabilitiesOf(R"AXEA(struct Point
{
    i32 x
} i32 set(Point p)
{ p.x = 5  return p.x } p = Point { x: 1 } x = set(p))AXEA");
    EXPECT_TRUE(capabilities.at("set")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a parameter whose field is incremented")
{
    const auto capabilities = capabilitiesOf(R"AXEA(struct Point
{
    i32 x
} i32 bump(Point p)
{ p.x++  return p.x } p = Point { x: 1 } x = bump(p))AXEA");
    EXPECT_TRUE(capabilities.at("bump")[0] == Capability::Write);
}

TEST("CapabilityChecker does not require write for incrementing a plain (non-field) parameter")
{
    const auto capabilities = capabilitiesOf(R"AXEA(i32 bump(i32 n)
{ n++  return n } x = bump(1))AXEA");
    EXPECT_TRUE(capabilities.at("bump")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a parameter propagated through an inherent struct "
     "method call - a user method's own mutation-of-self isn't knowable by name alone, unlike "
     "the hardcoded builtin collection method names (see docs/language/0006-generics.md's own "
     "generic-methods follow-up)")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Counter
{
    i32 value

    void increment(self)
    { self.value = self.value + 1 }
} void bump(Counter c)
{ c.increment() } c = Counter { value: 0 } bump(c))AXEA");
    EXPECT_TRUE(capabilities.at("bump")[0] == Capability::Write);
}

TEST("CapabilityChecker infers take when propagated through a call")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Packet
{
    i32 id
} i32 send(take Packet packet)
{ return packet.id } i32 relay(Packet packet)
{ return send(packet) } p = Packet { id: 1 } x = relay(p))AXEA");
    EXPECT_TRUE(capabilities.at("relay")[0] == Capability::Take);
}

TEST("CapabilityChecker infers write when propagated through a call")
{
    const auto capabilities = capabilitiesOf(R"AXEA(struct Point
{
    i32 x
} i32 set(Point p)
{ p.x = 1  return p.x } i32 wrapper(Point p)
{ return set(p) } p = Point { x: 1 } x = wrapper(p))AXEA");
    EXPECT_TRUE(capabilities.at("wrapper")[0] == Capability::Write);
}

TEST("CapabilityChecker accepts an explicit declaration at or above the inferred minimum")
{
    const auto capabilities = capabilitiesOf(R"AXEA(struct Point
{
    i32 x
} i32 get(read Point p)
{ return p.x } p = Point { x: 1 } x = get(p))AXEA");
    EXPECT_TRUE(capabilities.at("get")[0] == Capability::Read);
}

TEST("CapabilityChecker rejects an explicit declaration weaker than what the body needs")
{
    const std::string source = R"AXEA(struct Point
{
    i32 x
} i32 set(read Point p)
{ p.x = 5  return p.x } p = Point { x: 1 } x = set(p))AXEA";
    EXPECT_THROWS(capabilitiesOf(source));
}

TEST("CapabilityChecker rejects use of a value after it has been taken")
{
    const std::string source = R"AXEA(struct Packet
{
    i32 id
} i32 send(take Packet packet)
{ return packet.id } i32 relay(Packet packet)
{   a = send(packet)   b = send(packet)   return a + b } p = Packet { id: 1 } x = relay(p))AXEA";
    EXPECT_THROWS(capabilitiesOf(source));
}

TEST("CapabilityChecker scopes move-checking to a single block: a move in one branch "
     "does not affect the sibling branch or code after the if")
{
    const std::string source = R"AXEA(struct Packet
{
    i32 id
} i32 send(take Packet packet)
{ return packet.id } i32 relay(Packet packet, bool flag)
{   if flag { send(packet) } else { 0 }   return send(packet) } p = Packet { id: 1 } x = relay(p, true))AXEA";
    capabilitiesOf(source); // must not throw, per the documented per-block limitation
}

TEST("CapabilityChecker infers write for a parameter whose field is mutated inside a loop")
{
    const auto capabilities = capabilitiesOf(R"AXEA(struct Counter
{
    i32 value
} i32 bump(Counter c)
{   n = 0   while n < 3 { c.value++  n = n + 1 }   return c.value } c = Counter { value: 0 } x = bump(c))AXEA");
    EXPECT_TRUE(capabilities.at("bump")[0] == Capability::Write);
}

TEST("CapabilityChecker does not track a move as persisting across loop iterations")
{
    // Same already-documented per-block limitation as if/else, extended to
    // loops: each iteration's move-tracking starts fresh (see
    // docs/language/0028-loops.md), so this must not throw even though a
    // real dataflow analysis would flag the second iteration's use.
    const std::string source = R"AXEA(struct Packet
{
    i32 id
} i32 send(take Packet packet)
{ return packet.id } i32 relay(Packet packet)
{   n = 0   while n < 2 { send(packet)  n = n + 1 }   return n } p = Packet { id: 1 } x = relay(p))AXEA";
    capabilitiesOf(source);
}

TEST("CapabilityChecker infers write for a parameter whose element is index-assigned")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(i32 bump([i32; 3] values)
{ values[0] = 99  return values[0] } x = bump([1, 2, 3]))AXEA");
    EXPECT_TRUE(capabilities.at("bump")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a parameter that is only indexed for reading")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(i32 sum([i32; 3] values)
{ return values[0] + values[1] + values[2] } x = sum([1, 2, 3]))AXEA");
    EXPECT_TRUE(capabilities.at("sum")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a slice<T> parameter whose element is index-assigned")
{
    // Regression insurance that the existing type-agnostic IndexExpr-walking
    // mechanism (added for arrays) really does apply to slice<T> too, not
    // just in theory - see docs/language/0032-slices.md.
    const auto capabilities = capabilitiesOf(R"AXEA(void zeroFirst(slice<i32> values)
{ values[0] = 0 } a = [1, 2, 3] called = zeroFirst(a))AXEA");
    EXPECT_TRUE(capabilities.at("zeroFirst")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a slice<T> parameter that is only indexed for reading")
{
    const auto capabilities = capabilitiesOf(R"AXEA(i32 sum(slice<i32> values)
{ return values[0] } x = sum([1, 2, 3]))AXEA");
    EXPECT_TRUE(capabilities.at("sum")[0] == Capability::Read);
}

// List<T> is a real, user-declared generic struct now (see docs/language/0006-generics.md's own
// List<T> port follow-up and std/collections.ax) - its own push/pop/get capability inference is
// exercised through the general struct method dispatch mechanism instead (see "CapabilityChecker
// infers write for a parameter propagated through an inherent struct method call" above), using
// a small inline generic struct here rather than `use`-ing the real module (this file's own
// `capabilitiesOf` helper parses a single in-memory string, with no module loader involved).
TEST("CapabilityChecker infers write for a generic struct parameter mutated through an "
     "inherent method call")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    T value

    void set(self, T v)
    { self.value = v }
} void appendOne(Box<i32> b)
{ c = b.set(1) } a = Box<i32> { value: 0 } called = appendOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("appendOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a generic struct parameter only read through an "
     "inherent method call")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    T value

    T get(self)
    { return self.value }
} i32 first(Box<i32> b)
{ return b.get() } a = Box<i32> { value: 1 } x = first(a))AXEA");
    EXPECT_TRUE(capabilities.at("first")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a Stack<T>-shaped struct parameter that is pushed to")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    void push(self, T value)
    { }
} void pushOne(Box<i32> s)
{ s.push(1) } a = Box<i32> { length: 0 } called = pushOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("pushOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Stack<T>-shaped struct parameter that is popped")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    T pop(self)
    { return self.length }
} i32 popOne(Box<i32> s)
{ return s.pop() } a = Box<i32> { length: 0 } x = popOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("popOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a Stack<T>-shaped struct parameter that is only peeked")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    T peek(self)
    { return self.length }
} i32 peekOne(Box<i32> s)
{ return s.peek() } a = Box<i32> { length: 0 } x = peekOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("peekOne")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a LinkedList<T>-shaped struct parameter that is "
     "push_front'd")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    void push_front(self, T value)
    { }
} void pushOne(Box<i32> s)
{ s.push_front(1) } a = Box<i32> { length: 0 } called = pushOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("pushOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a LinkedList<T>-shaped struct parameter that is "
     "push_back'd")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    void push_back(self, T value)
    { }
} void pushOne(Box<i32> s)
{ s.push_back(1) } a = Box<i32> { length: 0 } called = pushOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("pushOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a LinkedList<T>-shaped struct parameter that is "
     "pop_front'd")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    T pop_front(self)
    { return self.length }
} i32 popOne(Box<i32> s)
{ return s.pop_front() } a = Box<i32> { length: 0 } x = popOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("popOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a LinkedList<T>-shaped struct parameter that is "
     "pop_back'd")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    T pop_back(self)
    { return self.length }
} i32 popOne(Box<i32> s)
{ return s.pop_back() } a = Box<i32> { length: 0 } x = popOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("popOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Deque<T>-shaped struct parameter that is "
     "push_back'd")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    void push_back(self, T value)
    { }
} void pushOne(Box<i32> d)
{ d.push_back(1) } a = Box<i32> { length: 0 } called = pushOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("pushOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Deque<T>-shaped struct parameter that is pop_front'd")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    T pop_front(self)
    { return self.length }
} i32 popOne(Box<i32> d)
{ return d.pop_front() } a = Box<i32> { length: 0 } x = popOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("popOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Deque<T>-shaped struct parameter whose element is "
     "set")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    T get(self, i32 index)
    { return self.length }

    void set(self, i32 index, T value)
    { }
} i32 bump(Box<i32> d)
{ d.set(0, 99)  return d.get(0) } a = Box<i32> { length: 0 } x = bump(a))AXEA");
    EXPECT_TRUE(capabilities.at("bump")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a Deque<T>-shaped struct parameter that is only "
     "read via .get")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    T get(self, i32 index)
    { return self.length }
} i32 first(Box<i32> d)
{ return d.get(0) } a = Box<i32> { length: 0 } x = first(a))AXEA");
    EXPECT_TRUE(capabilities.at("first")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a Queue<T>-shaped struct parameter that is enqueue'd")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    void enqueue(self, T value)
    { }
} void pushOne(Box<i32> q)
{ q.enqueue(1) } a = Box<i32> { length: 0 } called = pushOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("pushOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Queue<T>-shaped struct parameter that is dequeue'd")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    T dequeue(self)
    { return self.length }
} i32 popOne(Box<i32> q)
{ return q.dequeue() } a = Box<i32> { length: 0 } x = popOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("popOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a PriorityQueue<T>-shaped struct parameter that is "
     "pushed to")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    void push(self, T value)
    { }
} void pushOne(Box<i32> q)
{ q.push(1) } a = Box<i32> { length: 0 } called = pushOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("pushOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a PriorityQueue<T>-shaped struct parameter that is "
     "popped")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    T pop(self)
    { return self.length }
} i32 popOne(Box<i32> q)
{ return q.pop() } a = Box<i32> { length: 0 } x = popOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("popOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a PriorityQueue<T>-shaped struct parameter that is "
     "only peeked")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    T peek(self)
    { return self.length }
} i32 peekOne(Box<i32> q)
{ return q.peek() } a = Box<i32> { length: 0 } x = peekOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("peekOne")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a Map<K,V>-shaped struct parameter that is 'set'")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<K,V>
{
    i32 length

    void set(self, K key, V value)
    { }
} void put(Box<i32,i32> m)
{ m.set(1, 2) } a = Box<i32,i32> { length: 0 } called = put(a))AXEA");
    EXPECT_TRUE(capabilities.at("put")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Map<K,V>-shaped struct parameter that is 'remove'd")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<K,V>
{
    i32 length

    void remove(self, K key)
    { }
} void drop(Box<i32,i32> m)
{ m.remove(1) } a = Box<i32,i32> { length: 0 } called = drop(a))AXEA");
    EXPECT_TRUE(capabilities.at("drop")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a Map<K,V>-shaped struct parameter that is only "
     "'get'/'contains'")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<K,V>
{
    i32 length

    bool contains(self, K key)
    { return true }
} bool peek(Box<i32,i32> m)
{ return m.contains(1) } a = Box<i32,i32> { length: 0 } x = peek(a))AXEA");
    EXPECT_TRUE(capabilities.at("peek")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a Set<T>-shaped struct parameter that is 'add'ed to")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    void add(self, T value)
    { }
} void addOne(Box<i32> s)
{ s.add(1) } a = Box<i32> { length: 0 } called = addOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("addOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a Set<T>-shaped struct parameter that is only 'contains'")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    bool contains(self, T value)
    { return true }
} bool peek(Box<i32> s)
{ return s.contains(1) } a = Box<i32> { length: 0 } x = peek(a))AXEA");
    EXPECT_TRUE(capabilities.at("peek")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a SortedMap<K,V>-shaped struct parameter that is 'set'")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<K,V>
{
    i32 length

    void set(self, K key, V value)
    { }
} void put(Box<i32,i32> m)
{ m.set(1, 2) } a = Box<i32,i32> { length: 0 } called = put(a))AXEA");
    EXPECT_TRUE(capabilities.at("put")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a SortedMap<K,V>-shaped struct parameter that is "
     "'remove'd")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<K,V>
{
    i32 length

    void remove(self, K key)
    { }
} void drop(Box<i32,i32> m)
{ m.remove(1) } a = Box<i32,i32> { length: 0 } called = drop(a))AXEA");
    EXPECT_TRUE(capabilities.at("drop")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a SortedMap<K,V>-shaped struct parameter that is only "
     "'get'/'contains'")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<K,V>
{
    i32 length

    bool contains(self, K key)
    { return true }
} bool peek(Box<i32,i32> m)
{ return m.contains(1) } a = Box<i32,i32> { length: 0 } x = peek(a))AXEA");
    EXPECT_TRUE(capabilities.at("peek")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a SortedSet<T>-shaped struct parameter that is 'add'ed "
     "to")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    void add(self, T value)
    { }
} void addOne(Box<i32> s)
{ s.add(1) } a = Box<i32> { length: 0 } called = addOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("addOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a SortedSet<T>-shaped struct parameter that is "
     "'remove'd")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    void remove(self, T value)
    { }
} void drop(Box<i32> s)
{ s.remove(1) } a = Box<i32> { length: 0 } called = drop(a))AXEA");
    EXPECT_TRUE(capabilities.at("drop")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a SortedSet<T>-shaped struct parameter that is only "
     "'contains'")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Box<T>
{
    i32 length

    bool contains(self, T value)
    { return true }
} bool peek(Box<i32> s)
{ return s.contains(1) } a = Box<i32> { length: 0 } x = peek(a))AXEA");
    EXPECT_TRUE(capabilities.at("peek")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a String parameter that is 'append'ed to")
{
    const auto capabilities = capabilitiesOf(R"AXEA(void addOne(String s)
{ s.append("x") } a = String("a") called = addOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("addOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a String parameter that is only read via .length")
{
    const auto capabilities = capabilitiesOf(R"AXEA(i32 peek(String s)
{ return s.length } a = String("a") called = a.append("b") x = peek(a))AXEA");
    EXPECT_TRUE(capabilities.at("peek")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a Buffer parameter that is 'append'ed to")
{
    const auto capabilities = capabilitiesOf(R"AXEA(void addOne(Buffer b)
{ b.append("x") } a = Buffer() called = addOne(a))AXEA");
    EXPECT_TRUE(capabilities.at("addOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Buffer parameter that is 'append_line'd to")
{
    const auto capabilities = capabilitiesOf(R"AXEA(void addLine(Buffer b)
{ b.append_line("x") } a = Buffer() called = addLine(a))AXEA");
    EXPECT_TRUE(capabilities.at("addLine")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Buffer parameter that is 'clear'ed")
{
    const auto capabilities = capabilitiesOf(R"AXEA(void wipe(Buffer b)
{ b.clear() } a = Buffer() called = wipe(a))AXEA");
    EXPECT_TRUE(capabilities.at("wipe")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Buffer parameter that is 'reserve'd")
{
    const auto capabilities = capabilitiesOf(R"AXEA(void grow(Buffer b)
{ b.reserve(8) } a = Buffer() called = grow(a))AXEA");
    EXPECT_TRUE(capabilities.at("grow")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Buffer parameter that is 'finish'ed")
{
    const auto capabilities = capabilitiesOf(R"AXEA(String done(Buffer b)
{ return b.finish() } a = Buffer() s = done(a))AXEA");
    EXPECT_TRUE(capabilities.at("done")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a Buffer parameter that is only read via .length/.capacity")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(i32 peek(Buffer b)
{ return b.length + b.capacity } a = Buffer() called = a.append("b") x = peek(a))AXEA");
    EXPECT_TRUE(capabilities.at("peek")[0] == Capability::Read);
}

TEST("CapabilityChecker infers read for a char parameter - a plain value with no mutating "
     "methods, same default every scalar type already gets")
{
    const auto capabilities = capabilitiesOf(R"AXEA(char identity(char c)
{ return c } x = identity('A'))AXEA");
    EXPECT_TRUE(capabilities.at("identity")[0] == Capability::Read);
}

TEST("CapabilityChecker infers read for a str parameter that is only sliced, never mutated - "
     "slicing produces a fresh copy, same as every other read-only operation")
{
    const auto capabilities = capabilitiesOf(R"AXEA(str firstFour(str d)
{ return d[..4] } date = "2026-08-18" x = firstFour(date))AXEA");
    EXPECT_TRUE(capabilities.at("firstFour")[0] == Capability::Read);
}

TEST("CapabilityChecker infers read for a str parameter that is only parsed, never mutated - "
     "parse<T>() is read-only, same as every other read-only operation")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(Optional<i32> toInt(str d)
{ return d.parse<i32>() } x = toInt("42"))AXEA");
    EXPECT_TRUE(capabilities.at("toInt")[0] == Capability::Read);
}

TEST("CapabilityChecker infers read for a str parameter whose .length/.bytes are read - field "
     "reads never raise write, same as every other field access")
{
    const auto capabilities = capabilitiesOf(R"AXEA(i32 count(str d)
{ return d.length + d.bytes } x = count("hello"))AXEA");
    EXPECT_TRUE(capabilities.at("count")[0] == Capability::Read);
}

TEST("CapabilityChecker infers read for a str parameter passed through .to_cstr() to an extern "
     "call - extern calls never raise write, no capability inference needed for FFI-safe types")
{
    const auto capabilities =
        capabilitiesOf("extern c puts(text: cstr) "
                       "greet(name: str) -> i32 { called = puts(name.to_cstr())  return 1 } "
                       "x = greet(\"hi\")");
    EXPECT_TRUE(capabilities.at("greet")[0] == Capability::Read);
}

TEST("CapabilityChecker infers read for a str parameter only passed to print/write - the "
     "builtin call arguments are read-only, same as every other read-only operation")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(i32 greet(str name)
{ print(name) return 1 } x = greet("hi"))AXEA");
    EXPECT_TRUE(capabilities.at("greet")[0] == Capability::Read);
}

TEST("CapabilityChecker infers read for a str parameter only used inside an interpolation span - "
     "interpolation pieces are read-only, same as any other expression use")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(String greet(str name)
{ return "hi {name}" } x = greet("a"))AXEA");
    EXPECT_TRUE(capabilities.at("greet")[0] == Capability::Read);
}

TEST("CapabilityChecker infers read for an Array parameter that is only joined - .join() never "
     "mutates its object")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(String describe([i32; 4] nums)
{ return nums.join(",") } x = describe([1, 2, 3, 4]))AXEA");
    EXPECT_TRUE(capabilities.at("describe")[0] == Capability::Read);
}

TEST("CapabilityChecker's move-only closure capture requires Capability::Take on a captured "
     "struct-typed param of the enclosing function (see docs/language/0067-closures.md) - "
     "unconditionally, regardless of what the closure body does with its own copy afterward")
{
    const auto capabilities = capabilitiesOf(R"AXEA(struct Point
{
    i32 x
} fn() -> i32 makeGetter(Point p)
{   return fn() -> i32 { return p.x } } getter = makeGetter(Point { x: 5 }))AXEA");
    EXPECT_TRUE(capabilities.at("makeGetter")[0] == Capability::Take);
}

TEST("CapabilityChecker infers a struct-typed closure *parameter*'s own capability from how the "
     "closure body uses it, exactly like a real top-level function's param - keyed by the "
     "closure literal's own synthetic 'closure$N' identity (see "
     "docs/language/0067-closures.md's own registerClosure and closureEffectiveCapabilities). "
     "Nested inside a real function body: CapabilityChecker (like RegionChecker) only ever "
     "analyzes a *function's own* body, never bare top-level script statements - this exact "
     "closure literal, written as a top-level assignment with no enclosing function, would never "
     "be discovered at all.")
{
    const auto capabilities =
        capabilitiesOf(R"AXEA(struct Point
{
    i32 x
} i32 run()
{   get: fn(Point) -> i32 = fn(p: Point) -> i32 { return p.x }   return get(Point { x: 5 }) } y = run())AXEA");
    EXPECT_TRUE(capabilities.at("closure$0")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a struct-typed closure parameter whose own field is "
     "mutated inside the closure body")
{
    const auto capabilities = capabilitiesOf(R"AXEA(struct Point
{
    i32 x
} i32 run()
{   bump: fn(Point) -> i32 = fn(p: Point) -> i32 {     p.x = p.x + 1     return p.x   }   return bump(Point { x: 5 }) } y = run())AXEA");
    EXPECT_TRUE(capabilities.at("closure$0")[0] == Capability::Write);
}

TEST("CapabilityChecker rejects an explicitly-declared closure parameter capability that's too "
     "weak for how the closure's own body actually uses it - the identical error a real "
     "under-declared function parameter already gets")
{
    EXPECT_THROWS(capabilitiesOf(R"AXEA(struct Point
{
    i32 x
} i32 run()
{   bump = fn(read p: Point) -> i32 {     p.x = p.x + 1     return p.x   }   return bump(Point { x: 5 }) } y = run())AXEA"));
}

TEST("CapabilityChecker rejects using a struct-typed closure parameter again after it's already "
     "been `take`n inside the same closure body - the closure's own body gets an independent "
     "move-check, exactly like a real top-level function's body")
{
    EXPECT_THROWS(capabilitiesOf(R"AXEA(struct Point
{
    i32 x
} i32 consume(take Point pt)
{ return pt.x } i32 run()
{   consumeTwice = fn(take p: Point) -> i32 {     a = consume(p)     b = consume(p)     return a + b   }   return consumeTwice(Point { x: 5 }) } y = run())AXEA"));
}

TEST("CapabilityChecker's move-only capture rejects capturing the same struct-typed local into "
     "two different closures - the second capture is a use of an already-moved value, mirroring "
     "exactly how a second `take`-consuming call on the same value is already rejected")
{
    EXPECT_THROWS(capabilitiesOf(R"AXEA(struct Point
{
    i32 x
    i32 y
} i32 run()
{   p = Point { x: 1, y: 2 }   a: fn() -> i32 = fn() -> i32 { return p.x }   b: fn() -> i32 = fn() -> i32 { return p.y }   return a() + b() } y = run())AXEA"));
}

TEST("CapabilityChecker does not raise a pointer parameter's capability for writing through it "
     "via '*ptr = value' - *T deliberately never participates in capability inference (see "
     "docs/language/0019-unsafe.md)")
{
    auto capabilities = capabilitiesOf(R"AXEA(void f(*i32 ptr)
{ unsafe { *ptr = 5 } })AXEA");
    EXPECT_TRUE(capabilities.at("f")[0] == Capability::Read);
}
