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
                                             "get(p: Point) -> i32 { p.x } "
                                             "p = Point { x: 1 } "
                                             "x = get(p)");
    EXPECT_TRUE(capabilities.at("get")[0] == Capability::Read);
}

TEST("CapabilityChecker infers write for a parameter whose field is assigned")
{
    const auto capabilities = capabilitiesOf("struct Point { x: i32 } "
                                             "set(p: Point) -> i32 { p.x = 5  p.x } "
                                             "p = Point { x: 1 } "
                                             "x = set(p)");
    EXPECT_TRUE(capabilities.at("set")[0] == Capability::Write);
}

TEST("CapabilityChecker infers write for a parameter whose field is incremented")
{
    const auto capabilities = capabilitiesOf("struct Point { x: i32 } "
                                             "bump(p: Point) -> i32 { p.x++  p.x } "
                                             "p = Point { x: 1 } "
                                             "x = bump(p)");
    EXPECT_TRUE(capabilities.at("bump")[0] == Capability::Write);
}

TEST("CapabilityChecker does not require write for incrementing a plain (non-field) parameter")
{
    const auto capabilities = capabilitiesOf("bump(n: i32) -> i32 { n++  n } "
                                             "x = bump(1)");
    EXPECT_TRUE(capabilities.at("bump")[0] == Capability::Read);
}

TEST("CapabilityChecker infers take when propagated through a call")
{
    const auto capabilities = capabilitiesOf("struct Packet { id: i32 } "
                                             "send(take packet: Packet) -> i32 { packet.id } "
                                             "relay(packet: Packet) -> i32 { send(packet) } "
                                             "p = Packet { id: 1 } "
                                             "x = relay(p)");
    EXPECT_TRUE(capabilities.at("relay")[0] == Capability::Take);
}

TEST("CapabilityChecker infers write when propagated through a call")
{
    const auto capabilities = capabilitiesOf("struct Point { x: i32 } "
                                             "set(p: Point) -> i32 { p.x = 1  p.x } "
                                             "wrapper(p: Point) -> i32 { set(p) } "
                                             "p = Point { x: 1 } "
                                             "x = wrapper(p)");
    EXPECT_TRUE(capabilities.at("wrapper")[0] == Capability::Write);
}

TEST("CapabilityChecker accepts an explicit declaration at or above the inferred minimum")
{
    const auto capabilities = capabilitiesOf("struct Point { x: i32 } "
                                             "get(read p: Point) -> i32 { p.x } "
                                             "p = Point { x: 1 } "
                                             "x = get(p)");
    EXPECT_TRUE(capabilities.at("get")[0] == Capability::Read);
}

TEST("CapabilityChecker rejects an explicit declaration weaker than what the body needs")
{
    const std::string source = "struct Point { x: i32 } "
                               "set(read p: Point) -> i32 { p.x = 5  p.x } "
                               "p = Point { x: 1 } "
                               "x = set(p)";
    EXPECT_THROWS(capabilitiesOf(source));
}

TEST("CapabilityChecker rejects use of a value after it has been taken")
{
    const std::string source = "struct Packet { id: i32 } "
                               "send(take packet: Packet) -> i32 { packet.id } "
                               "relay(packet: Packet) -> i32 { "
                               "  a = send(packet) "
                               "  b = send(packet) "
                               "  a + b "
                               "} "
                               "p = Packet { id: 1 } "
                               "x = relay(p)";
    EXPECT_THROWS(capabilitiesOf(source));
}

TEST("CapabilityChecker scopes move-checking to a single block: a move in one branch "
     "does not affect the sibling branch or code after the if")
{
    const std::string source = "struct Packet { id: i32 } "
                               "send(take packet: Packet) -> i32 { packet.id } "
                               "relay(packet: Packet, flag: bool) -> i32 { "
                               "  if flag { send(packet) } else { 0 } "
                               "  send(packet) "
                               "} "
                               "p = Packet { id: 1 } "
                               "x = relay(p, true)";
    capabilitiesOf(source); // must not throw, per the documented per-block limitation
}
