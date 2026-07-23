#include <RTSEngine/Roguelite/RunSimulationArchive.h>
#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {

using namespace rts;

void check(bool condition) {
    assert(condition);
    if (!condition) std::abort();
}

void configureHistoryRun(
    roguelite::RunSimulation& simulation,
    bool createActors) {
    simulation.setResources(100);
    simulation.setPlayerTeam(1);

    gameplay::UnitDefinition regular;
    regular.id = 1;
    regular.cellsPerTick = 1;
    regular.combat = {10, 0, 1, 1, 1, 3};
    simulation.registerUnit(regular);

    gameplay::UnitDefinition bossUnit;
    bossUnit.id = 2;
    bossUnit.cellsPerTick = 1;
    bossUnit.combat = {30, 1, 4, 1, 2, 10};
    simulation.registerUnit(bossUnit);

    check(simulation.registerAffix(
        {101, 1, {1500, 1, 1000, 1000, 2000}}));
    check(simulation.registerBoss(
        {201, 2, 1, 1, {2000, 2, 1250, 1000, 1500}}));
    check(simulation.registerLane({1, {0, 2}, {10, 2}, 1}));

    roguelite::ModifierDefinition salvage;
    salvage.id = 1;
    salvage.weight = 1;
    salvage.maxStacks = 1;
    salvage.effects = {
        {roguelite::WaveCompletionResourceStat(),
         roguelite::ModifierOperation::Add,
         5}
    };
    check(simulation.registerModifier(salvage));

    tower_defense::WaveDefinition first;
    first.id = 1;
    first.budget = 1;
    first.spawnIntervalTicks = 1;
    first.laneIds = {1};
    first.enemies = {{1, 1, 1, 1}};
    first.rewardPool = {1};
    first.rewardChoices = 1;
    check(simulation.registerWave(first));

    tower_defense::WaveDefinition second;
    second.id = 2;
    second.budget = 1;
    second.spawnIntervalTicks = 1;
    second.laneIds = {1};
    second.bossPool = {201};
    second.bossCount = 1;
    second.affixPool = {101};
    second.affixChoices = 1;
    second.rewardChoices = 0;
    check(simulation.registerWave(second));
    check(simulation.registerRun({7, {1, 2}}));

    if (createActors) {
        simulation.createBaseCore(
            {10, 2}, 1, {500, 0, 0, 0, 1, 0});
        simulation.createDefender(
            {7, 2}, {0}, 1, {500, 0, 100, 4, 1, 0});
    }
}

std::uint64_t advanceToFirstReward(
    roguelite::RunSimulation& simulation) {
    check(simulation.submit(
        {0, 1, 1, roguelite::CommandType::StartRun, 7}));
    for (std::uint64_t tick = 0; tick < 80; ++tick) {
        check(simulation.step(tick));
        if (simulation.state().phase == roguelite::RunPhase::RewardPending) {
            return tick;
        }
    }
    std::abort();
}

void checkFirstWaveRewardHistory(
    const roguelite::RunSimulation& simulation) {
    const auto& history = simulation.history();
    check(history.runId == 7);
    check(history.phase == roguelite::RunHistoryPhase::Active);
    check(!history.legacyImported);
    check(history.waves.size() == 1);

    const auto& wave = history.waves.front();
    check(wave.waveId == 1);
    check(wave.waveIndex == 0);
    check(wave.phase == roguelite::WaveResultPhase::RewardPending);
    check(wave.plannedEnemies == 1);
    check(wave.plannedBosses == 0);
    check(wave.enemiesDefeated == 1);
    check(wave.bossesDefeated == 0);
    check(wave.coreHealthStart == 500);
    check(wave.coreHealthEnd == 500);
    check(wave.coreHealthMaximum == 500);
    check(wave.resourcesStart == 100);
    check(wave.resourcesEnd == 103);
    check(wave.resourceDelta == 3);
    check(wave.resourceBonus == 0);
    check(wave.affixes.empty());
    check(wave.bosses.empty());
    check(wave.rewardChoices == std::vector<roguelite::ModifierId>({1}));
    check(wave.selectedModifier == 0);
    check(!wave.modifierApplied);
}

std::uint64_t legacyNextWaveTick(
    const roguelite::RunSimulation& simulation) {
    const auto& value = simulation.waveSequence().state();
    const auto noTick =
        tower_defense::WaveSequenceDirector::noScheduledTick();
    switch (value.phase) {
    case tower_defense::WaveSequencePhase::Idle:
        return 0;
    case tower_defense::WaveSequencePhase::Preparing:
        return value.scheduledStartTick;
    case tower_defense::WaveSequencePhase::Failed:
        if (simulation.tower().director().state().phase ==
                tower_defense::WavePhase::Failed ||
            value.preparationStartedTick == noTick) {
            return noTick;
        }
        return value.waveIndex == 0
            ? value.preparationStartedTick
            : value.preparationStartedTick + 1u;
    default:
        return noTick;
    }
}

std::uint64_t legacyWorldHash(
    const roguelite::RunSimulation& simulation,
    std::uint32_t nextInternalSequence,
    std::uint32_t playerTeamId) {
    const auto& snapshot = simulation.snapshot();
    const auto& state = simulation.state();
    const auto profile = roguelite::ResolveGameplayProfile(
        simulation.modifiers());

    foundation::CanonicalHash hash;
    hash.WriteU64(simulation.lastTick());
    hash.WriteU64(simulation.rootSeed());
    hash.WriteU64(snapshot.towerDefenseWorldHash);
    hash.WriteU32(state.runId);
    hash.WriteU8(static_cast<std::uint8_t>(state.phase));
    hash.WriteU32(state.waveIndex);
    hash.WriteU32(state.completedWaves);
    hash.WriteU32(state.currentWave);
    hash.WriteU64(legacyNextWaveTick(simulation));
    hash.WriteU32(nextInternalSequence);
    hash.WriteI32(snapshot.availableResources);
    hash.WriteI32(simulation.modifiers().resolve(
        roguelite::WaveCompletionResourceStat(), 0));
    hash.WriteU32(playerTeamId);
    hash.WriteI32(profile.unitHealth);
    hash.WriteI32(profile.unitDamage);
    hash.WriteI32(profile.unitArmorAdd);
    hash.WriteI32(profile.unitMoveSpeed);
    hash.WriteI32(profile.buildingHealth);
    hash.WriteI32(profile.buildingDamage);
    hash.WriteI32(profile.constructionSpeed);
    hash.WriteI32(profile.productionSpeed);
    hash.WriteI32(profile.bountyMultiplier);

    const auto commandState = simulation.commandStreamState();
    hash.WriteU64(commandState.committedThrough);
    hash.WriteU32(static_cast<std::uint32_t>(commandState.pending.size()));
    for (const auto& command : commandState.pending) {
        hash.WriteU64(command.targetTick);
        hash.WriteU32(command.issuer);
        hash.WriteU32(command.sequence);
        hash.WriteU8(static_cast<std::uint8_t>(command.type));
        hash.WriteU32(command.objectId);
    }

    const auto* run = simulation.waveSequence().definition(state.runId);
    hash.WriteBool(run != nullptr);
    if (run) {
        hash.WriteU32(static_cast<std::uint32_t>(run->waves.size()));
        for (const auto waveId : run->waves) hash.WriteU32(waveId);
    }
    simulation.modifiers().appendHash(hash);
    return hash.Value();
}

bool skipHistory(foundation::BinaryReader& reader) {
    std::uint32_t value32 = 0;
    std::uint64_t value64 = 0;
    std::int32_t signed32 = 0;
    std::uint8_t value8 = 0;
    bool flag = false;
    std::uint32_t waveCount = 0;
    if (!reader.readU32(value32) ||
        !reader.readU64(value64) ||
        !reader.readU64(value64) ||
        !reader.readU8(value8) ||
        !reader.readBool(flag) ||
        !reader.readU32(waveCount)) {
        return false;
    }
    for (std::uint32_t wave = 0; wave < waveCount; ++wave) {
        if (!reader.readU32(value32) ||
            !reader.readU32(value32) ||
            !reader.readU64(value64) ||
            !reader.readU64(value64) ||
            !reader.readU8(value8)) {
            return false;
        }
        for (int index = 0; index < 4; ++index) {
            if (!reader.readU32(value32)) return false;
        }
        for (int index = 0; index < 7; ++index) {
            if (!reader.readI32(signed32)) return false;
        }
        for (int group = 0; group < 3; ++group) {
            std::uint32_t count = 0;
            if (!reader.readU32(count)) return false;
            for (std::uint32_t index = 0; index < count; ++index) {
                if (!reader.readU32(value32)) return false;
            }
        }
        if (!reader.readU32(value32) || !reader.readBool(flag)) return false;
    }
    return true;
}

std::vector<std::uint8_t> makeLegacyV1(
    const roguelite::RunSimulation& simulation) {
    const auto current = roguelite::EncodeRunSimulation(simulation);
    check(!current.empty());
    foundation::BinaryReader reader(current);
    foundation::BinaryWriter writer;

    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint64_t contentHash = 0;
    std::uint32_t towerCount = 0;
    std::vector<std::uint8_t> towerBytes;
    check(reader.readU32(magic));
    check(reader.readU16(version));
    check(reader.readU64(contentHash));
    check(version == 2u);
    check(reader.readU32(towerCount));
    check(reader.readBytes(
        towerCount, towerBytes,
        roguelite::RunSimulationArchive::kMaximumTowerBytes));
    writer.writeU32(magic);
    writer.writeU16(1u);
    writer.writeU64(contentHash);
    writer.writeU32(towerCount);
    writer.writeBytes(towerBytes);

    std::uint32_t count = 0;
    check(reader.readU32(count));
    writer.writeU32(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t id = 0;
        std::uint32_t stacks = 0;
        check(reader.readU32(id));
        check(reader.readU32(stacks));
        writer.writeU32(id);
        writer.writeU32(stacks);
    }

    std::uint64_t committedThrough = 0;
    check(reader.readU64(committedThrough));
    check(reader.readU32(count));
    writer.writeU64(committedThrough);
    writer.writeU32(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint64_t targetTick = 0;
        std::uint32_t issuer = 0;
        std::uint32_t sequence = 0;
        std::uint8_t type = 0;
        std::uint32_t objectId = 0;
        check(reader.readU64(targetTick));
        check(reader.readU32(issuer));
        check(reader.readU32(sequence));
        check(reader.readU8(type));
        check(reader.readU32(objectId));
        writer.writeU64(targetTick);
        writer.writeU32(issuer);
        writer.writeU32(sequence);
        writer.writeU8(type);
        writer.writeU32(objectId);
    }

    std::uint32_t runId = 0;
    std::uint8_t phase = 0;
    std::uint32_t waveIndex = 0;
    std::uint32_t completedWaves = 0;
    std::uint32_t currentWave = 0;
    std::uint64_t rootSeed = 0;
    std::uint64_t nextWaveTick = 0;
    std::uint64_t lastTick = 0;
    std::uint32_t nextInternalSequence = 0;
    std::uint32_t playerTeamId = 0;
    bool hasStepped = false;
    check(reader.readU32(runId));
    check(reader.readU8(phase));
    check(reader.readU32(waveIndex));
    check(reader.readU32(completedWaves));
    check(reader.readU32(currentWave));
    check(reader.readU64(rootSeed));
    check(reader.readU64(nextWaveTick));
    check(reader.readU64(lastTick));
    check(reader.readU32(nextInternalSequence));
    check(reader.readU32(playerTeamId));
    check(reader.readBool(hasStepped));
    writer.writeU32(runId);
    writer.writeU8(phase);
    writer.writeU32(waveIndex);
    writer.writeU32(completedWaves);
    writer.writeU32(currentWave);
    writer.writeU64(rootSeed);
    writer.writeU64(nextWaveTick);
    writer.writeU64(lastTick);
    writer.writeU32(nextInternalSequence);
    writer.writeU32(playerTeamId);
    writer.writeBool(hasStepped);

    check(skipHistory(reader));
    std::uint64_t currentWorldHash = 0;
    check(reader.readU64(currentWorldHash));
    check(reader.atEnd());
    (void)currentWorldHash;
    writer.writeU64(legacyWorldHash(
        simulation, nextInternalSequence, playerTeamId));
    return writer.take();
}

void testHistoryRoundTripAndCompletion() {
    roguelite::RunSimulation original(12, 5, 0x8181u);
    configureHistoryRun(original, true);
    const auto rewardTick = advanceToFirstReward(original);
    checkFirstWaveRewardHistory(original);

    const auto bytes = roguelite::EncodeRunSimulation(original);
    check(!bytes.empty());
    roguelite::RunSimulation restored(12, 5, 0x8181u);
    configureHistoryRun(restored, false);
    check(roguelite::DecodeRunSimulation(bytes, restored));
    check(restored.snapshot().worldHash == original.snapshot().worldHash);
    check(restored.history() == original.history());

    const roguelite::TickCommand choose{
        rewardTick + 1u, 1, 2,
        roguelite::CommandType::ChooseModifier, 1};
    check(original.submit(choose));
    check(restored.submit(choose));

    bool completed = false;
    for (std::uint64_t tick = rewardTick + 1u; tick < 180; ++tick) {
        check(original.step(tick));
        check(restored.step(tick));
        check(original.snapshot().worldHash == restored.snapshot().worldHash);
        check(original.history() == restored.history());
        if (original.state().phase == roguelite::RunPhase::Complete) {
            completed = true;
            break;
        }
    }
    check(completed);

    const auto& history = original.history();
    check(history.phase == roguelite::RunHistoryPhase::Complete);
    check(history.finishedTick >= history.startedTick);
    check(history.waves.size() == 2);

    const auto& first = history.waves[0];
    check(first.phase == roguelite::WaveResultPhase::Complete);
    check(first.selectedModifier == 1);
    check(first.modifierApplied);

    const auto& second = history.waves[1];
    check(second.waveId == 2);
    check(second.waveIndex == 1);
    check(second.phase == roguelite::WaveResultPhase::Complete);
    check(second.plannedEnemies == 1);
    check(second.plannedBosses == 1);
    check(second.enemiesDefeated == 1);
    check(second.bossesDefeated == 1);
    check(second.affixes ==
          std::vector<tower_defense::WaveAffixId>({101}));
    check(second.bosses ==
          std::vector<tower_defense::BossId>({201}));
    check(second.coreHealthStart == 500);
    check(second.coreHealthEnd == 500);
    check(second.resourcesStart == 103);
    check(second.resourcesEnd == 138);
    check(second.resourceDelta == 35);
    check(second.resourceBonus == 5);
    check(second.rewardChoices.empty());
    check(second.selectedModifier == 0);
    check(!second.modifierApplied);
    check(original.snapshot().availableResources == 138);
    check(roguelite::EncodeRunSimulation(original) ==
          roguelite::EncodeRunSimulation(restored));
}

void testLegacyV1Migration() {
    roguelite::RunSimulation source(12, 5, 0x8282u);
    configureHistoryRun(source, true);
    const auto rewardTick = advanceToFirstReward(source);
    const auto bytes = makeLegacyV1(source);
    check(!bytes.empty());

    foundation::BinaryReader header(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    check(header.readU32(magic));
    check(header.readU16(version));
    check(magic == roguelite::RunSimulationArchive::kMagic);
    check(version == 1u);

    roguelite::RunSimulation first(12, 5, 0x8282u);
    roguelite::RunSimulation second(12, 5, 0x8282u);
    configureHistoryRun(first, false);
    configureHistoryRun(second, false);
    check(roguelite::DecodeRunSimulation(bytes, first));
    check(roguelite::DecodeRunSimulation(bytes, second));
    check(first.snapshot().worldHash == second.snapshot().worldHash);
    check(first.tower().snapshot().worldHash ==
          source.tower().snapshot().worldHash);
    check(first.state().phase == roguelite::RunPhase::RewardPending);
    check(first.history().legacyImported);
    check(first.history() == second.history());
    check(first.history().waves.size() == 1);
    check(first.history().waves.front().rewardChoices ==
          std::vector<roguelite::ModifierId>({1}));

    const roguelite::TickCommand choose{
        rewardTick + 1u, 1, 2,
        roguelite::CommandType::ChooseModifier, 1};
    check(first.submit(choose));
    check(second.submit(choose));
    bool completed = false;
    for (std::uint64_t tick = rewardTick + 1u; tick < 180; ++tick) {
        check(first.step(tick));
        check(second.step(tick));
        check(first.snapshot().worldHash == second.snapshot().worldHash);
        check(first.history() == second.history());
        if (first.state().phase == roguelite::RunPhase::Complete) {
            completed = true;
            break;
        }
    }
    check(completed);
    check(first.history().legacyImported);
    check(first.history().phase == roguelite::RunHistoryPhase::Complete);
    check(first.history().waves.size() == 2);
}

void testFailedWaveIsSealed() {
    roguelite::RunSimulation simulation(8, 5, 0x9191u);

    gameplay::UnitDefinition enemy;
    enemy.id = 1;
    enemy.cellsPerTick = 1;
    enemy.combat = {100, 0, 20, 1, 1, 0};
    simulation.registerUnit(enemy);
    check(simulation.registerLane({1, {0, 2}, {6, 2}, 1}));

    tower_defense::WaveDefinition wave;
    wave.id = 1;
    wave.budget = 1;
    wave.laneIds = {1};
    wave.enemies = {{1, 1, 1, 1}};
    wave.rewardChoices = 0;
    check(simulation.registerWave(wave));
    check(simulation.registerRun({1, {1}}));
    simulation.createBaseCore(
        {6, 2}, 1, {5, 0, 0, 0, 1, 0});

    check(simulation.submit(
        {0, 1, 1, roguelite::CommandType::StartRun, 1}));
    bool failed = false;
    for (std::uint64_t tick = 0; tick < 80; ++tick) {
        check(simulation.step(tick));
        if (simulation.state().phase == roguelite::RunPhase::Failed) {
            failed = true;
            break;
        }
    }
    check(failed);
    const auto& history = simulation.history();
    check(history.phase == roguelite::RunHistoryPhase::Failed);
    check(history.finishedTick != 0);
    check(history.waves.size() == 1);
    check(history.waves.front().phase ==
          roguelite::WaveResultPhase::Failed);
    check(history.waves.front().coreHealthStart == 5);
    check(history.waves.front().coreHealthEnd == 0);
    check(history.waves.front().resourcesStart == 0);
    check(history.waves.front().resourcesEnd == 0);
    check(history.waves.front().resourceDelta == 0);
}

} // namespace

int main() {
    testHistoryRoundTripAndCompletion();
    testLegacyV1Migration();
    testFailedWaveIsSealed();
    std::cout << "roguelite history tests passed\n";
    return 0;
}
