#include <rts/sim/DeterministicCommandStream.h>

#include <cstdlib>
#include <cstdint>
#include <iostream>

namespace {

void require(bool condition) {
    if (!condition) std::abort();
}

struct Command {
    std::uint64_t targetTick{};
    std::uint32_t issuer{};
    std::uint32_t sequence{};
    std::uint32_t payload{};
};

void testOrderingAndDuplicates() {
    rts::sim::DeterministicCommandStream<Command> stream;
    require(stream.submit({5, 2, 2, 22}));
    require(stream.submit({5, 1, 2, 12}));
    require(stream.submit({5, 1, 1, 11}));
    require(stream.submitDetailed({5, 1, 1, 11}) ==
            rts::sim::CommandSubmitResult::Accepted);
    require(stream.pending() == 3);
    require(stream.submitDetailed({5, 1, 1, 99}) ==
            rts::sim::CommandSubmitResult::DuplicateIdentity);
    require(stream.pending() == 3);

    const auto commands = stream.consume(5);
    require(commands.size() == 3);
    require(commands[0].issuer == 1 && commands[0].sequence == 1);
    require(commands[0].payload == 11);
    require(commands[1].issuer == 1 && commands[1].sequence == 2);
    require(commands[2].issuer == 2 && commands[2].sequence == 2);
    require(stream.pending() == 0);
    require(stream.committedThrough() == 6);
    require(stream.submitDetailed({4, 3, 1, 0}) ==
            rts::sim::CommandSubmitResult::Late);
}

void testSkippedTicksDiscardStalePendingCommands() {
    rts::sim::DeterministicCommandStream<Command> stream;
    require(stream.submit({1, 1, 1, 10}));
    require(stream.submit({3, 1, 2, 30}));

    const auto skipped = stream.consume(2);
    require(skipped.empty());
    require(stream.pending() == 1);
    require(stream.submitDetailed({1, 1, 3, 0}) ==
            rts::sim::CommandSubmitResult::Late);

    const auto future = stream.consume(3);
    require(future.size() == 1);
    require(future.front().payload == 30);
    require(stream.consume(2).empty());
}

} // namespace

int main() {
    testOrderingAndDuplicates();
    testSkippedTicksDiscardStalePendingCommands();
    std::cout << "deterministic command stream tests passed\n";
    return 0;
}
