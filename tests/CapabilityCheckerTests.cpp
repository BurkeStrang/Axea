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
