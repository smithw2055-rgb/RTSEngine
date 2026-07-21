#include <iostream>

#include <rts/foundation/CanonicalHash.h>
#include <rts/foundation/Random.h>
#include <rts/sim/SimulationHost.h>

int main() {
    rts::foundation::RandomStream waveRandom(42, rts::foundation::MakeRandomStreamId("wave"));
    rts::foundation::CanonicalHash hash;

    rts::sim::SimulationHost host([&](const rts::sim::TickContext& context) {
        hash.WriteU64(context.tick);
        hash.WriteU32(waveRandom.NextBounded(1000));
    });

    for (int frame = 0; frame < 120; ++frame) {
        host.AdvanceFrame(1.0 / 60.0);
    }

    std::cout << "tick=" << host.CurrentTick() << '\n';
    std::cout << "hash=" << hash.Value() << '\n';
    return host.CurrentTick() == 60 ? 0 : 1;
}
