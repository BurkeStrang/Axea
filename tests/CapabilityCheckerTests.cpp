#include "TestFramework.hpp"

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

        TypeChecker typeChecker;
        typeChecker.check(program);

        CapabilityChecker capabilityChecker;
        capabilityChecker.check(program);
        return capabilityChecker.effectiveCapabilities();
    }
} // namespace

TEST("CapabilityChecker infers read for a parameter that is only read")
{
    const auto capabilities = capabilitiesOf("struct Point { x: i32 } "
                                             "get(p: Point) -> i32 { return p.x } "
                                             "p = Point { x: 1 } "
                                             "x = get(p)");
    EXPECT_TRUE(capabilities.at("get")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a parameter whose field is assigned")
{
    const auto capabilities = capabilitiesOf("struct Point { x: i32 } "
                                             "set(p: Point) -> i32 { p.x = 5  return p.x } "
                                             "p = Point { x: 1 } "
                                             "x = set(p)");
    EXPECT_TRUE(capabilities.at("set")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a parameter whose field is incremented")
{
    const auto capabilities = capabilitiesOf("struct Point { x: i32 } "
                                             "bump(p: Point) -> i32 { p.x++  return p.x } "
                                             "p = Point { x: 1 } "
                                             "x = bump(p)");
    EXPECT_TRUE(capabilities.at("bump")[0] == Capability::Write);
}

TEST("CapabilityChecker does not require write for incrementing a plain (non-field) parameter")
{
    const auto capabilities = capabilitiesOf("bump(n: i32) -> i32 { n++  return n } "
                                             "x = bump(1)");
    EXPECT_TRUE(capabilities.at("bump")[0] == Capability::Read);
}

TEST("CapabilityChecker infers take when propagated through a call")
{
    const auto capabilities =
        capabilitiesOf("struct Packet { id: i32 } "
                       "send(take packet: Packet) -> i32 { return packet.id } "
                       "relay(packet: Packet) -> i32 { return send(packet) } "
                       "p = Packet { id: 1 } "
                       "x = relay(p)");
    EXPECT_TRUE(capabilities.at("relay")[0] == Capability::Take);
}

TEST("CapabilityChecker infers write when propagated through a call")
{
    const auto capabilities = capabilitiesOf("struct Point { x: i32 } "
                                             "set(p: Point) -> i32 { p.x = 1  return p.x } "
                                             "wrapper(p: Point) -> i32 { return set(p) } "
                                             "p = Point { x: 1 } "
                                             "x = wrapper(p)");
    EXPECT_TRUE(capabilities.at("wrapper")[0] == Capability::Write);
}

TEST("CapabilityChecker accepts an explicit declaration at or above the inferred minimum")
{
    const auto capabilities = capabilitiesOf("struct Point { x: i32 } "
                                             "get(read p: Point) -> i32 { return p.x } "
                                             "p = Point { x: 1 } "
                                             "x = get(p)");
    EXPECT_TRUE(capabilities.at("get")[0] == Capability::Read);
}

TEST("CapabilityChecker rejects an explicit declaration weaker than what the body needs")
{
    const std::string source = "struct Point { x: i32 } "
                               "set(read p: Point) -> i32 { p.x = 5  return p.x } "
                               "p = Point { x: 1 } "
                               "x = set(p)";
    EXPECT_THROWS(capabilitiesOf(source));
}

TEST("CapabilityChecker rejects use of a value after it has been taken")
{
    const std::string source = "struct Packet { id: i32 } "
                               "send(take packet: Packet) -> i32 { return packet.id } "
                               "relay(packet: Packet) -> i32 { "
                               "  a = send(packet) "
                               "  b = send(packet) "
                               "  return a + b "
                               "} "
                               "p = Packet { id: 1 } "
                               "x = relay(p)";
    EXPECT_THROWS(capabilitiesOf(source));
}

TEST("CapabilityChecker scopes move-checking to a single block: a move in one branch "
     "does not affect the sibling branch or code after the if")
{
    const std::string source = "struct Packet { id: i32 } "
                               "send(take packet: Packet) -> i32 { return packet.id } "
                               "relay(packet: Packet, flag: bool) -> i32 { "
                               "  if flag { send(packet) } else { 0 } "
                               "  return send(packet) "
                               "} "
                               "p = Packet { id: 1 } "
                               "x = relay(p, true)";
    capabilitiesOf(source); // must not throw, per the documented per-block limitation
}

TEST("CapabilityChecker infers write for a parameter whose field is mutated inside a loop")
{
    const auto capabilities = capabilitiesOf("struct Counter { value: i32 } "
                                             "bump(c: Counter) -> i32 { "
                                             "  n = 0 "
                                             "  while n < 3 { c.value++  n = n + 1 } "
                                             "  return c.value "
                                             "} "
                                             "c = Counter { value: 0 } "
                                             "x = bump(c)");
    EXPECT_TRUE(capabilities.at("bump")[0] == Capability::Write);
}

TEST("CapabilityChecker does not track a move as persisting across loop iterations")
{
    // Same already-documented per-block limitation as if/else, extended to
    // loops: each iteration's move-tracking starts fresh (see
    // docs/language/0028-loops.md), so this must not throw even though a
    // real dataflow analysis would flag the second iteration's use.
    const std::string source = "struct Packet { id: i32 } "
                               "send(take packet: Packet) -> i32 { return packet.id } "
                               "relay(packet: Packet) -> i32 { "
                               "  n = 0 "
                               "  while n < 2 { send(packet)  n = n + 1 } "
                               "  return n "
                               "} "
                               "p = Packet { id: 1 } "
                               "x = relay(p)";
    capabilitiesOf(source);
}

TEST("CapabilityChecker infers write for a parameter whose element is index-assigned")
{
    const auto capabilities =
        capabilitiesOf("bump(values: [i32; 3]) -> i32 { values[0] = 99  return values[0] } "
                       "x = bump([1, 2, 3])");
    EXPECT_TRUE(capabilities.at("bump")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a parameter that is only indexed for reading")
{
    const auto capabilities =
        capabilitiesOf("sum(values: [i32; 3]) -> i32 { return values[0] + values[1] + values[2] } "
                       "x = sum([1, 2, 3])");
    EXPECT_TRUE(capabilities.at("sum")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a slice<T> parameter whose element is index-assigned")
{
    // Regression insurance that the existing type-agnostic IndexExpr-walking
    // mechanism (added for arrays) really does apply to slice<T> too, not
    // just in theory - see docs/language/0032-slices.md.
    const auto capabilities = capabilitiesOf("zeroFirst(values: slice<i32>) { values[0] = 0 } "
                                             "a = [1, 2, 3] "
                                             "called = zeroFirst(a)");
    EXPECT_TRUE(capabilities.at("zeroFirst")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a slice<T> parameter that is only indexed for reading")
{
    const auto capabilities = capabilitiesOf("sum(values: slice<i32>) -> i32 { return values[0] } "
                                             "x = sum([1, 2, 3])");
    EXPECT_TRUE(capabilities.at("sum")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a List<T> parameter that is pushed to")
{
    const auto capabilities = capabilitiesOf("appendOne(numbers: List<i32>) { numbers.push(1) } "
                                             "a = List<i32>() "
                                             "called = appendOne(a)");
    EXPECT_TRUE(capabilities.at("appendOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a List<T> parameter that is popped")
{
    const auto capabilities =
        capabilitiesOf("removeOne(numbers: List<i32>) -> i32 { return numbers.pop() } "
                       "a = List<i32>() "
                       "called = a.push(1) "
                       "x = removeOne(a)");
    EXPECT_TRUE(capabilities.at("removeOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a List<T> parameter that is only indexed for reading")
{
    const auto capabilities =
        capabilitiesOf("first(numbers: List<i32>) -> i32 { return numbers[0] } "
                       "a = List<i32>() "
                       "called = a.push(1) "
                       "x = first(a)");
    EXPECT_TRUE(capabilities.at("first")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a Stack<T> parameter that is pushed to")
{
    const auto capabilities = capabilitiesOf("pushOne(s: Stack<i32>) { s.push(1) } "
                                             "a = Stack<i32>() "
                                             "called = pushOne(a)");
    EXPECT_TRUE(capabilities.at("pushOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Stack<T> parameter that is popped")
{
    const auto capabilities = capabilitiesOf("popOne(s: Stack<i32>) -> i32 { return s.pop() } "
                                             "a = Stack<i32>() "
                                             "called = a.push(1) "
                                             "x = popOne(a)");
    EXPECT_TRUE(capabilities.at("popOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a Stack<T> parameter that is only peeked")
{
    const auto capabilities = capabilitiesOf("peekOne(s: Stack<i32>) -> i32 { return s.peek() } "
                                             "a = Stack<i32>() "
                                             "called = a.push(1) "
                                             "x = peekOne(a)");
    EXPECT_TRUE(capabilities.at("peekOne")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a LinkedList<T> parameter that is push_front'd")
{
    const auto capabilities = capabilitiesOf("pushOne(s: LinkedList<i32>) { s.push_front(1) } "
                                             "a = LinkedList<i32>() "
                                             "called = pushOne(a)");
    EXPECT_TRUE(capabilities.at("pushOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a LinkedList<T> parameter that is push_back'd")
{
    const auto capabilities = capabilitiesOf("pushOne(s: LinkedList<i32>) { s.push_back(1) } "
                                             "a = LinkedList<i32>() "
                                             "called = pushOne(a)");
    EXPECT_TRUE(capabilities.at("pushOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a LinkedList<T> parameter that is pop_front'd")
{
    const auto capabilities =
        capabilitiesOf("popOne(s: LinkedList<i32>) -> i32 { return s.pop_front() } "
                       "a = LinkedList<i32>() "
                       "called = a.push_front(1) "
                       "x = popOne(a)");
    EXPECT_TRUE(capabilities.at("popOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a LinkedList<T> parameter that is pop_back'd")
{
    const auto capabilities =
        capabilitiesOf("popOne(s: LinkedList<i32>) -> i32 { return s.pop_back() } "
                       "a = LinkedList<i32>() "
                       "called = a.push_front(1) "
                       "x = popOne(a)");
    EXPECT_TRUE(capabilities.at("popOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Deque<T> parameter that is push_back'd")
{
    const auto capabilities = capabilitiesOf("pushOne(d: Deque<i32>) { d.push_back(1) } "
                                             "a = Deque<i32>() "
                                             "called = pushOne(a)");
    EXPECT_TRUE(capabilities.at("pushOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Deque<T> parameter that is pop_front'd")
{
    const auto capabilities =
        capabilitiesOf("popOne(d: Deque<i32>) -> i32 { return d.pop_front() } "
                       "a = Deque<i32>() "
                       "called = a.push_back(1) "
                       "x = popOne(a)");
    EXPECT_TRUE(capabilities.at("popOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Deque<T> parameter whose element is index-assigned")
{
    // Regression insurance again (see the slice<T> test above) - the same
    // type-agnostic IndexExpr-walking mechanism applies to Deque<T> too,
    // with zero Deque-specific CapabilityChecker code (see
    // docs/language/0037-deques.md).
    const auto capabilities =
        capabilitiesOf("bump(d: Deque<i32>) -> i32 { d[0] = 99  return d[0] } "
                       "a = Deque<i32>() "
                       "called = a.push_back(1) "
                       "x = bump(a)");
    EXPECT_TRUE(capabilities.at("bump")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a Deque<T> parameter that is only indexed for reading")
{
    const auto capabilities = capabilitiesOf("first(d: Deque<i32>) -> i32 { return d[0] } "
                                             "a = Deque<i32>() "
                                             "called = a.push_back(1) "
                                             "x = first(a)");
    EXPECT_TRUE(capabilities.at("first")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a Queue<T> parameter that is enqueue'd")
{
    const auto capabilities = capabilitiesOf("pushOne(q: Queue<i32>) { q.enqueue(1) } "
                                             "a = Queue<i32>() "
                                             "called = pushOne(a)");
    EXPECT_TRUE(capabilities.at("pushOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Queue<T> parameter that is dequeue'd")
{
    const auto capabilities = capabilitiesOf("popOne(q: Queue<i32>) -> i32 { return q.dequeue() } "
                                             "a = Queue<i32>() "
                                             "called = a.enqueue(1) "
                                             "x = popOne(a)");
    EXPECT_TRUE(capabilities.at("popOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a PriorityQueue<T> parameter that is pushed to")
{
    const auto capabilities = capabilitiesOf("pushOne(q: PriorityQueue<i32>) { q.push(1) } "
                                             "a = PriorityQueue<i32>() "
                                             "called = pushOne(a)");
    EXPECT_TRUE(capabilities.at("pushOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a PriorityQueue<T> parameter that is popped")
{
    const auto capabilities =
        capabilitiesOf("popOne(q: PriorityQueue<i32>) -> i32 { return q.pop() } "
                       "a = PriorityQueue<i32>() "
                       "called = a.push(1) "
                       "x = popOne(a)");
    EXPECT_TRUE(capabilities.at("popOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a PriorityQueue<T> parameter that is only peeked")
{
    const auto capabilities =
        capabilitiesOf("peekOne(q: PriorityQueue<i32>) -> i32 { return q.peek() } "
                       "a = PriorityQueue<i32>() "
                       "called = a.push(1) "
                       "x = peekOne(a)");
    EXPECT_TRUE(capabilities.at("peekOne")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a Map<i32,i32> parameter that is 'set'")
{
    const auto capabilities = capabilitiesOf("put(m: Map<i32,i32>) { m.set(1, 2) } "
                                             "a = Map<i32,i32>() "
                                             "called = put(a)");
    EXPECT_TRUE(capabilities.at("put")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Map<i32,i32> parameter that is 'remove'd")
{
    const auto capabilities = capabilitiesOf("drop(m: Map<i32,i32>) { m.remove(1) } "
                                             "a = Map<i32,i32>() "
                                             "called = drop(a)");
    EXPECT_TRUE(capabilities.at("drop")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a Map<i32,i32> parameter that is only 'get'/'contains'")
{
    const auto capabilities =
        capabilitiesOf("peek(m: Map<i32,i32>) -> bool { return m.contains(1) } "
                       "a = Map<i32,i32>() "
                       "called = a.set(1, 2) "
                       "x = peek(a)");
    EXPECT_TRUE(capabilities.at("peek")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a Set<i32> parameter that is 'add'ed to")
{
    const auto capabilities = capabilitiesOf("addOne(s: Set<i32>) { s.add(1) } "
                                             "a = Set<i32>() "
                                             "called = addOne(a)");
    EXPECT_TRUE(capabilities.at("addOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a Set<i32> parameter that is only 'contains'")
{
    const auto capabilities = capabilitiesOf("peek(s: Set<i32>) -> bool { return s.contains(1) } "
                                             "a = Set<i32>() "
                                             "called = a.add(1) "
                                             "x = peek(a)");
    EXPECT_TRUE(capabilities.at("peek")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a SortedMap<i32,i32> parameter that is 'set'")
{
    const auto capabilities = capabilitiesOf("put(m: SortedMap<i32,i32>) { m.set(1, 2) } "
                                             "a = SortedMap<i32,i32>() "
                                             "called = put(a)");
    EXPECT_TRUE(capabilities.at("put")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a SortedMap<i32,i32> parameter that is 'remove'd")
{
    const auto capabilities = capabilitiesOf("drop(m: SortedMap<i32,i32>) { m.remove(1) } "
                                             "a = SortedMap<i32,i32>() "
                                             "called = drop(a)");
    EXPECT_TRUE(capabilities.at("drop")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a SortedMap<i32,i32> parameter that is only "
     "'get'/'contains'")
{
    const auto capabilities =
        capabilitiesOf("peek(m: SortedMap<i32,i32>) -> bool { return m.contains(1) } "
                       "a = SortedMap<i32,i32>() "
                       "called = a.set(1, 2) "
                       "x = peek(a)");
    EXPECT_TRUE(capabilities.at("peek")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a SortedSet<i32> parameter that is 'add'ed to")
{
    const auto capabilities = capabilitiesOf("addOne(s: SortedSet<i32>) { s.add(1) } "
                                             "a = SortedSet<i32>() "
                                             "called = addOne(a)");
    EXPECT_TRUE(capabilities.at("addOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a SortedSet<i32> parameter that is 'remove'd")
{
    const auto capabilities = capabilitiesOf("drop(s: SortedSet<i32>) { s.remove(1) } "
                                             "a = SortedSet<i32>() "
                                             "called = drop(a)");
    EXPECT_TRUE(capabilities.at("drop")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a SortedSet<i32> parameter that is only 'contains'")
{
    const auto capabilities =
        capabilitiesOf("peek(s: SortedSet<i32>) -> bool { return s.contains(1) } "
                       "a = SortedSet<i32>() "
                       "called = a.add(1) "
                       "x = peek(a)");
    EXPECT_TRUE(capabilities.at("peek")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a String parameter that is 'append'ed to")
{
    const auto capabilities = capabilitiesOf("addOne(s: String) { s.append(\"x\") } "
                                             "a = String(\"a\") "
                                             "called = addOne(a)");
    EXPECT_TRUE(capabilities.at("addOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a String parameter that is only read via .length")
{
    const auto capabilities = capabilitiesOf("peek(s: String) -> i32 { return s.length } "
                                             "a = String(\"a\") "
                                             "called = a.append(\"b\") "
                                             "x = peek(a)");
    EXPECT_TRUE(capabilities.at("peek")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a Buffer parameter that is 'append'ed to")
{
    const auto capabilities = capabilitiesOf("addOne(b: Buffer) { b.append(\"x\") } "
                                             "a = Buffer() "
                                             "called = addOne(a)");
    EXPECT_TRUE(capabilities.at("addOne")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Buffer parameter that is 'append_line'd to")
{
    const auto capabilities = capabilitiesOf("addLine(b: Buffer) { b.append_line(\"x\") } "
                                             "a = Buffer() "
                                             "called = addLine(a)");
    EXPECT_TRUE(capabilities.at("addLine")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Buffer parameter that is 'clear'ed")
{
    const auto capabilities = capabilitiesOf("wipe(b: Buffer) { b.clear() } "
                                             "a = Buffer() "
                                             "called = wipe(a)");
    EXPECT_TRUE(capabilities.at("wipe")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Buffer parameter that is 'reserve'd")
{
    const auto capabilities = capabilitiesOf("grow(b: Buffer) { b.reserve(8) } "
                                             "a = Buffer() "
                                             "called = grow(a)");
    EXPECT_TRUE(capabilities.at("grow")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a Buffer parameter that is 'finish'ed")
{
    const auto capabilities = capabilitiesOf("done(b: Buffer) -> String { return b.finish() } "
                                             "a = Buffer() "
                                             "s = done(a)");
    EXPECT_TRUE(capabilities.at("done")[0] == Capability::Write);
}

TEST("CapabilityChecker infers read for a Buffer parameter that is only read via .length/.capacity")
{
    const auto capabilities =
        capabilitiesOf("peek(b: Buffer) -> i32 { return b.length + b.capacity } "
                       "a = Buffer() "
                       "called = a.append(\"b\") "
                       "x = peek(a)");
    EXPECT_TRUE(capabilities.at("peek")[0] == Capability::Read);
}

TEST("CapabilityChecker infers read for a char parameter - a plain value with no mutating "
     "methods, same default every scalar type already gets")
{
    const auto capabilities = capabilitiesOf("identity(c: char) -> char { return c } "
                                             "x = identity('A')");
    EXPECT_TRUE(capabilities.at("identity")[0] == Capability::Read);
}

TEST("CapabilityChecker infers read for a str parameter that is only sliced, never mutated - "
     "slicing produces a fresh copy, same as every other read-only operation")
{
    const auto capabilities = capabilitiesOf("firstFour(d: str) -> str { return d[..4] } "
                                             "date = \"2026-08-18\" "
                                             "x = firstFour(date)");
    EXPECT_TRUE(capabilities.at("firstFour")[0] == Capability::Read);
}

TEST("CapabilityChecker infers read for a str parameter that is only parsed, never mutated - "
     "parse<T>() is read-only, same as every other read-only operation")
{
    const auto capabilities = capabilitiesOf("toInt(d: str) -> i32 { return d.parse<i32>() } "
                                             "x = toInt(\"42\")");
    EXPECT_TRUE(capabilities.at("toInt")[0] == Capability::Read);
}

TEST("CapabilityChecker infers read for a str parameter whose .length/.bytes are read - field "
     "reads never raise write, same as every other field access")
{
    const auto capabilities = capabilitiesOf("count(d: str) -> i32 { return d.length + d.bytes } "
                                             "x = count(\"hello\")");
    EXPECT_TRUE(capabilities.at("count")[0] == Capability::Read);
}
