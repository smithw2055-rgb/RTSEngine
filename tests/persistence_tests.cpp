#include <RTSEngine/Roguelite/RunSaveSchema.h>
#include <RTSEngine/Rts/Replay.h>
#include <RTSEngine/Rts/Simulation.h>
#include <rts/foundation/Random.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace rts;

void require(bool condition) {
    if (!condition) std::abort();
}

bool sameCommand(
    const gameplay::TickCommand& a,
    const gameplay::TickCommand& b) {
    return a.targetTick == b.targetTick &&
           a.issuer == b.issuer &&
           a.sequence == b.sequence &&
           a.type == b.type &&
           a.subject == b.subject &&
           a.targetX == b.targetX &&
           a.targetY == b.targetY &&
           a.append == b.append &&
           a.definitionId == b.definitionId &&
           a.objectId == b.objectId &&
           a.targetEntity == b.targetEntity;
}

void testRandomStateRestore() {
    const auto id = foundation::MakeRandomStreamId("save-test");
    foundation::RandomStream original(99, id);
    (void)original.NextU32();
    (void)original.NextU32();
    const auto state = original.Snapshot();
    const auto expected = original.NextU32();

    foundation::RandomStream restored(1, id);
    require(restored.Restore(state));
    require(restored.Snapshot() == state);
    require(restored.NextU32() == expected);

    auto invalid = state;
    invalid.increment &= ~std::uint64_t{1};
    require(!restored.Restore(invalid));
}

struct ReplayScenario {
    gameplay::RtsReplay replay;
    std::vector<sim::WorldHashCheckpoint> checkpoints;
};

ReplayScenario recordReplay() {
    gameplay::RtsSimulation simulation(10, 6);
    const auto unit = simulation.createUnit({0, 0}, {1});

    gameplay::TickCommand move;
    move.targetTick = 1;
    move.issuer = 1;
    move.sequence = 1;
    move.type = gameplay::CommandType::Move;
    move.subject = unit;
    move.targetX = 5;
    move.targetY = 2;

    gameplay::TickCommand stop;
    stop.targetTick = 9;
    stop.issuer = 1;
    stop.sequence = 2;
    stop.type = gameplay::CommandType::Stop;
    stop.subject = unit;

    gameplay::RtsReplayRecorder recorder;
    recorder.recordCommand(move);
    recorder.recordCommand(stop);
    require(simulation.submit(move));
    require(simulation.submit(stop));

    for (std::uint64_t tick = 0; tick <= 6; ++tick) {
        simulation.step(tick);
        recorder.recordCheckpoint(
            tick, simulation.snapshot().worldHash);
    }
    recorder.setCommandStreamState(simulation.commandStreamState());

    foundation::RandomStream random(
        1234, foundation::MakeRandomStreamId("replay.random"));
    (void)random.NextU32();
    recorder.setRandomStreams({random.Snapshot()});

    ReplayScenario result;
    result.replay = recorder.finish();
    result.checkpoints = result.replay.checkpoints;
    return result;
}

void testReplayRoundTripAndPlayback() {
    const auto recorded = recordReplay();
    const auto bytes = gameplay::EncodeRtsReplay(recorded.replay);
    require(!bytes.empty());

    gameplay::RtsReplay decoded;
    require(gameplay::DecodeRtsReplay(bytes, decoded));
    require(decoded.commands.size() == 2);
    require(decoded.checkpoints == recorded.checkpoints);
    require(decoded.randomStreams == recorded.replay.randomStreams);
    require(decoded.commandStream.committedThrough == 7);
    require(decoded.commandStream.pending.size() == 1);
    require(decoded.commandStream.pending.front().targetTick == 9);

    gameplay::RtsSimulation playback(10, 6);
    playback.createUnit({0, 0}, {1});
    for (const auto& command : decoded.commands) {
        require(playback.submit(command));
    }

    for (const auto& checkpoint : decoded.checkpoints) {
        playback.step(checkpoint.tick);
        require(playback.snapshot().worldHash == checkpoint.worldHash);
    }

    gameplay::TickCommandStream restoredStream;
    require(restoredStream.restore(decoded.commandStream));
    const auto restoredState = restoredStream.snapshot();
    require(restoredState.committedThrough ==
            decoded.commandStream.committedThrough);
    require(restoredState.pending.size() == 1);
    require(sameCommand(
        restoredState.pending.front(),
        decoded.commandStream.pending.front()));

    auto corrupted = bytes;
    corrupted.front() ^= 0xffu;
    gameplay::RtsReplay rejected;
    require(!gameplay::DecodeRtsReplay(corrupted, rejected));
}

void testRunSaveSchemaRoundTrip() {
    roguelite::RunSaveSchema save;
    save.tick = 42;
    save.rootSeed = 987654321;
    save.run = {7, roguelite::RunPhase::RewardPending, 2, 2, 30};
    save.resources = {125, 15, 300};
    save.gameplayProfile.unitHealth = 1500;
    save.gameplayProfile.unitDamage = 1250;
    save.gameplayProfile.constructionSpeed = 2000;
    save.gameplayProfile.productionSpeed = 1500;
    save.modifierStacks = {{3, 2}, {8, 1}};

    foundation::RandomStream random(
        save.rootSeed,
        foundation::MakeRandomStreamId("run.reward"));
    (void)random.NextU32();
    save.randomStreams = {random.Snapshot()};
    save.checkpoints = {{40, 1001}, {42, 2002}};

    save.runCommands.committedThrough = 43;
    save.runCommands.pending.push_back(
        {45, 1, 9, roguelite::CommandType::ChooseModifier, 8});
    save.towerCommands.committedThrough = 43;
    save.towerCommands.pending.push_back(
        {44, 0x80000001u, 3,
         tower_defense::CommandType::StartWave, 31});
    save.rtsCommands.committedThrough = 43;
    gameplay::TickCommand attackMove;
    attackMove.targetTick = 44;
    attackMove.issuer = 0x80000001u;
    attackMove.sequence = 4;
    attackMove.type = gameplay::CommandType::AttackMove;
    attackMove.subject = {5, 2};
    attackMove.targetX = 1;
    attackMove.targetY = 4;
    save.rtsCommands.pending.push_back(attackMove);

    const auto bytes = roguelite::EncodeRunSave(save);
    roguelite::RunSaveSchema decoded;
    require(roguelite::DecodeRunSave(bytes, decoded));
    require(decoded.tick == save.tick);
    require(decoded.rootSeed == save.rootSeed);
    require(decoded.run.runId == save.run.runId);
    require(decoded.run.phase == save.run.phase);
    require(decoded.run.waveIndex == save.run.waveIndex);
    require(decoded.run.completedWaves == save.run.completedWaves);
    require(decoded.run.currentWave == save.run.currentWave);
    require(decoded.resources.available == save.resources.available);
    require(decoded.resources.reserved == save.resources.reserved);
    require(decoded.resources.spent == save.resources.spent);
    require(decoded.gameplayProfile == save.gameplayProfile);
    require(decoded.modifierStacks == save.modifierStacks);
    require(decoded.randomStreams == save.randomStreams);
    require(decoded.checkpoints == save.checkpoints);
    require(decoded.runCommands.pending.size() == 1);
    require(decoded.towerCommands.pending.size() == 1);
    require(decoded.rtsCommands.pending.size() == 1);
    require(sameCommand(
        decoded.rtsCommands.pending.front(), attackMove));

    roguelite::TickCommandStream runStream;
    tower_defense::TickCommandStream towerStream;
    gameplay::TickCommandStream rtsStream;
    require(runStream.restore(decoded.runCommands));
    require(towerStream.restore(decoded.towerCommands));
    require(rtsStream.restore(decoded.rtsCommands));

    foundation::RandomStream restored(0, decoded.randomStreams.front().id);
    require(restored.Restore(decoded.randomStreams.front()));
    require(restored.Snapshot() == decoded.randomStreams.front());
}

} // namespace

int main() {
    testRandomStateRestore();
    testReplayRoundTripAndPlayback();
    testRunSaveSchemaRoundTrip();
    std::cout << "persistence tests passed\n";
    return 0;
}
