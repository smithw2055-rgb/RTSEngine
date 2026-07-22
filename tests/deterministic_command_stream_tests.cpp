#include <rts/sim/DeterministicCommandStream.h>

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

struct Command {
    std::uint64_t targetTick{};
    std::uint32_t issuer{};
    std::uint32_t sequence{};
    std::uint32_t payload{};
};

void testOrderingAndDuplicates() {
    rts::sim::DeterministicCommandStream<Command> stream;
    assert(stream.submit({5, 2, 2, 22}));
    assert(stream.submit({5, 1, 2, 12}));
    assert(stream.submit({5, 1, 1, 11}));
    assert(stream.submit({5, 1, 1, 99}));

    const auto commands = stream.consume(5);
    assert(commands.size() == 3);
    assert(commands[0].issuer == 1 && commands[0].sequence == 1);
    assert(commands[0].payload == 11);
    assert(commands[1].issuer == 1 && commands[1].sequence == 2);
    assert(commands[2].issuer == 2 && commands[2].sequence == 2);
    assert(stream.pending() == 0);
    assert(stream.committedThrough() == 6);
    assert(!stream.submit({4, 3, 1, 0}));
}

void testSkippedTicksDiscardStalePendingCommands() {
    rts::sim::DeterministicCommandStream<Command> stream;
    assert(stream.submit({1, 1, 1, 10}));
    assert(stream.submit({3, 1, 2, 30}));

    const auto skipped = stream.consume(2);
    assert(skipped.empty());
    assert(stream.pending() == 1);
    assert(!stream.submit({1, 1, 3, 0}));

    const auto future = stream.consume(3);
    assert(future.size() == 1);
    assert(future.front().payload == 30);
    assert(stream.consume(2).empty());
}

} // namespace

int main() {
    testOrderingAndDuplicates();
    testSkippedTicksDiscardStalePendingCommands();
    std::cout << "deterministic command stream tests passed\n";
    return 0;
}
