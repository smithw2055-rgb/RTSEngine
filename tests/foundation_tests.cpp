#include <cstdlib>
#include <iostream>

#include <rts/foundation/CanonicalHash.h>
#include <rts/foundation/Handle.h>
#include <rts/foundation/Random.h>
#include <rts/sim/SimulationHost.h>

namespace {
struct EntityTag {};

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main() {
    using rts::foundation::Handle;
    Require(!Handle<EntityTag>{}.IsValid(), "zero handle must be invalid");
    Require(Handle<EntityTag>{1, 1}.IsValid(), "non-zero handle must be valid");

    rts::foundation::CanonicalHash firstHash;
    firstHash.WriteU32(7);
    firstHash.WriteString("rts");
    rts::foundation::CanonicalHash secondHash;
    secondHash.WriteU32(7);
    secondHash.WriteString("rts");
    Require(firstHash.Value() == secondHash.Value(), "canonical hashes must match");

    rts::foundation::RandomStream waveA(123, rts::foundation::MakeRandomStreamId("wave"));
    rts::foundation::RandomStream waveB(123, rts::foundation::MakeRandomStreamId("wave"));
    rts::foundation::RandomStream combat(123, rts::foundation::MakeRandomStreamId("combat"));
    Require(waveA.NextU32() == waveB.NextU32(), "same random stream must reproduce");
    Require(waveA.NextU32() != combat.NextU32(), "named streams must be isolated");

    std::uint64_t observedTick = 0;
    rts::sim::SimulationHost host([&](const rts::sim::TickContext& context) {
        observedTick = context.tick;
    });
    const auto plan = host.AdvanceFrame(1.0);
    Require(plan.tickCount == 4, "clock must respect catch-up budget");
    Require(plan.alpha >= 0.0 && plan.alpha <= 1.0, "alpha must be clamped");
    Require(host.CurrentTick() == 4 && observedTick == 4, "host owns committed ticks");

    std::cout << "all foundation tests passed\n";
    return 0;
}
