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

using Stream = rts::sim::DeterministicCommandStream<Command>;

void testOrderingAndDuplicates() {
    Stream stream;
    require(stream.reserveTick(5, 8));
    require(stream.pending() == 0);
    require(stream.bucketCount() == 1);
    require(stream.submit({5, 2, 2, 22}));
    require(stream.submit({5, 1, 2, 12}));
    require(stream.submit({5, 1, 1, 11}));
    require(stream.submitDetailed({5, 1, 1, 11}) ==
            rts::sim::CommandSubmitResult::Accepted);
    require(stream.pending() == 3);
    require(stream.bucketCount() == 1);
    require(stream.submitDetailed({5, 1, 1, 99}) ==
            rts::sim::CommandSubmitResult::DuplicateIdentity);
    require(stream.pending() == 3);

    const auto commands = stream.consume(5);
    require(commands.size() == 3);
    require(commands.capacity() >= 8);
    require(commands[0].issuer == 1 && commands[0].sequence == 1);
    require(commands[0].payload == 11);
    require(commands[1].issuer == 1 && commands[1].sequence == 2);
    require(commands[2].issuer == 2 && commands[2].sequence == 2);
    require(stream.pending() == 0);
    require(stream.bucketCount() == 0);
    require(stream.committedThrough() == 6);
    require(!stream.reserveTick(4, 1));
    require(stream.submitDetailed({4, 3, 1, 0}) ==
            rts::sim::CommandSubmitResult::Late);
}

void testSkippedTicksDiscardStalePendingCommands() {
    Stream stream;
    require(stream.submit({1, 1, 1, 10}));
    require(stream.submit({3, 1, 2, 30}));
    require(stream.bucketCount() == 2);

    const auto skipped = stream.consume(2);
    require(skipped.empty());
    require(stream.pending() == 1);
    require(stream.bucketCount() == 1);
    require(stream.submitDetailed({1, 1, 3, 0}) ==
            rts::sim::CommandSubmitResult::Late);

    const auto future = stream.consume(3);
    require(future.size() == 1);
    require(future.front().payload == 30);
    require(stream.consume(2).empty());
}

void testSnapshotAndRestorePreserveCanonicalBuckets() {
    Stream stream;
    require(stream.submit({1000, 2, 2, 22}));
    require(stream.submit({7, 3, 1, 31}));
    require(stream.submit({1000, 1, 4, 14}));
    require(stream.submit({7, 1, 2, 12}));
    require(stream.submit({8, 1, 3, 13}));
    require(stream.bucketCount() == 3);

    const auto state = stream.snapshot();
    require(state.pending.size() == 5);
    require(state.pending[0].targetTick == 7);
    require(state.pending[0].issuer == 1);
    require(state.pending[1].targetTick == 7);
    require(state.pending[2].targetTick == 8);
    require(state.pending[3].targetTick == 1000);
    require(state.pending[3].issuer == 1);

    Stream restored;
    require(restored.restore(state));
    require(restored.pending() == 5);
    require(restored.bucketCount() == 3);

    const auto tickSeven = restored.consume(7);
    require(tickSeven.size() == 2);
    require(tickSeven[0].issuer == 1);
    require(tickSeven[1].issuer == 3);
    require(restored.pending() == 3);
    require(restored.bucketCount() == 2);

    const auto tickEight = restored.consume(8);
    require(tickEight.size() == 1);
    require(restored.pending() == 2);
    require(restored.bucketCount() == 1);

    // Consuming nearby Ticks does not move or rebuild the far-future bucket.
    require(restored.consume(9).empty());
    require(restored.pending() == 2);
    require(restored.bucketCount() == 1);
    const auto farFuture = restored.snapshot();
    require(farFuture.pending.size() == 2);
    require(farFuture.pending.front().targetTick == 1000);
}

void testRestoreRejectsConflictingIdentity() {
    Stream::State state;
    state.pending = {
        {5, 1, 1, 10},
        {5, 1, 1, 11}
    };
    Stream stream;
    require(!stream.restore(std::move(state)));
    require(stream.pending() == 0);
    require(stream.bucketCount() == 0);
}

} // namespace

int main() {
    testOrderingAndDuplicates();
    testSkippedTicksDiscardStalePendingCommands();
    testSnapshotAndRestorePreserveCanonicalBuckets();
    testRestoreRejectsConflictingIdentity();
    std::cout << "deterministic command stream tests passed\n";
    return 0;
}
