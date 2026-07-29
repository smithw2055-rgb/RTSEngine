#include <RTSEngine/Assets/AssetManager.h>
#include <RTSEngine/Assets/CookedAsset.h>
#include <RTSEngine/Assets/Vfs.h>
#include <RTSEngine/RtsScripting/RtsScriptLockstepSession.h>
#include <RTSEngine/Scripting/RealScriptHost.h>
#include <RTSEngine/Scripting/ScriptBundle.h>

#include <realscript/bytecode/Bytecode.h>
#include <realscript/game/GameProductization.h>
#include <rts/foundation/BinaryArchive.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace rts;
using namespace rts::gameplay;
namespace rs = rts::gameplay::scripting;
namespace sim = rts::sim;

#define require(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "RS3 assertion failed at line " << __LINE__          \
                      << ": " #condition "\n";                               \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

const char* source = R"(
module Game.PersistentAi;
import Engine.Rts;

class TeamBrain
{
    int decisions;

    void OnThink()
    {
        decisions = decisions + 1;
        long unit = FindIdleUnit();
        if (unit != 0)
        {
            bool queued = AttackMove(unit, decisions + 3, 2);
        }
    }
}
)";

void writeCooked(
    assets::MemoryVfs& vfs,
    const std::string& path,
    assets::CookedAsset asset) {
    const auto bytes = assets::EncodeCookedAsset(std::move(asset));
    require(!bytes.empty());
    require(vfs.write(path, bytes));
}

struct ScriptFixture final {
    assets::MemoryVfs vfs;
    assets::AssetManager assets{vfs, 32u * 1024u * 1024u};
    rs::RtsScriptApi api;
    rts::scripting::RealScriptHost host{assets, api.gameApi()};
    assets::AssetKey bundleKey{assets::AssetType::ScriptBundle, 930};
    std::shared_ptr<rts::scripting::ScriptProgram> program;

    ScriptFixture() {
        realscript::game::GameScriptCompiler compiler(api.gameApi());
        const auto compiled = compiler.compile({{"persistent_ai.rs", source}});
        require(compiled.succeeded());
        require(!compiled.modules.empty());

        std::vector<assets::CookedAsset> modules;
        std::uint64_t moduleId = 9300;
        for (const auto& module : compiled.modules) {
            modules.push_back(rts::scripting::ScriptAssetCodec::moduleAsset(
                moduleId++, realscript::bytecode::encodeModule(module)));
        }

        rts::scripting::ScriptDiagnostics diagnostics;
        const auto described = host.describeBundle(modules, diagnostics);
        require(described.has_value());
        require(!diagnostics.hasErrors());
        for (const auto& module : modules) {
            const auto path =
                "scripts/modules/" + std::to_string(module.key.id) + ".rta";
            writeCooked(vfs, path, module);
            require(assets.registerAsset({
                module.key,
                path,
                rts::scripting::kScriptModuleSchemaVersion}));
        }
        const auto bundle = rts::scripting::ScriptAssetCodec::bundleAsset(
            bundleKey.id, *described);
        writeCooked(vfs, "scripts/persistent_ai.rta", bundle);
        require(assets.registerAsset({
            bundleKey,
            "scripts/persistent_ai.rta",
            rts::scripting::kScriptBundleSchemaVersion}));
        const auto request = assets.request(bundleKey);
        require(request.valid());
        require(assets.process() == 1);
        const auto loaded = host.load(bundleKey);
        require(loaded.succeeded());
        program = loaded.program;
        require(program && program->valid());
        require(assets.releaseRequest(request));
    }
};

CombatStats combatProfile() {
    CombatStats result;
    result.maximumHealth = 50;
    result.weaponDamage = 3;
    result.weaponRange = 2;
    result.cooldownTicks = 1;
    return result;
}

void populateSession(RtsGameSession& session) {
    require(session.createUnit(
        {1, 2}, {1}, 1, combatProfile(), 8).valid());
}

rs::RtsTeamScriptDefinition definition() {
    rs::RtsTeamScriptDefinition value;
    value.teamId = 1;
    value.scriptType = "Game.PersistentAi::TeamBrain";
    value.thinkIntervalTicks = 1;
    value.maximumIntentsPerTick = 8;
    value.executionPolicy.instructionBudget = 10000;
    return value;
}

std::vector<std::uint8_t> firstObjectState(const rs::RtsScriptSession& scripts) {
    realscript::runtime::RuntimeError error;
    const auto bytes = scripts.encodeState(error);
    require(!bytes.empty());
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    std::uint32_t team = 0;
    std::string type;
    std::uint32_t interval = 0;
    std::uint64_t maximumIntents = 0;
    std::uint64_t instructionBudget = 0;
    std::uint64_t recursionLimit = 0;
    std::uint64_t gcWorkBudget = 0;
    std::uint8_t strict = 0;
    std::uint32_t sequence = 0;
    std::uint8_t enabled = 0;
    std::uint8_t started = 0;
    std::uint32_t objectSize = 0;
    std::vector<std::uint8_t> objectBytes;
    require(reader.readU32(magic));
    require(reader.readU32(version));
    require(reader.readU32(count) && count == 1);
    require(reader.readU32(team) && team == 1);
    require(reader.readString(type, 4096));
    require(reader.readU32(interval));
    require(reader.readU64(maximumIntents));
    require(reader.readU64(instructionBudget));
    require(reader.readU64(recursionLimit));
    require(reader.readU64(gcWorkBudget));
    require(reader.readU8(strict));
    require(reader.readU32(sequence));
    require(reader.readU8(enabled));
    require(reader.readU8(started));
    require(reader.readU32(objectSize) && objectSize != 0);
    require(reader.readBytes(objectSize, objectBytes, 16u * 1024u * 1024u));
    require(reader.atEnd());
    return objectBytes;
}

std::int64_t decisionCount(const rs::RtsScriptSession& scripts) {
    realscript::runtime::RuntimeError error;
    const auto state = realscript::game::decodeScriptObjectState(
        firstObjectState(scripts), error);
    require(state.has_value());
    for (const auto& field : state->fields) {
        if (field.name == "decisions") {
            return std::get<std::int64_t>(field.value);
        }
    }
    require(false);
    return -1;
}

std::uint64_t combinedHash(
    const RtsGameSession& session,
    const rs::RtsScriptSession& scripts) {
    std::uint64_t hash = 0;
    realscript::runtime::RuntimeError error;
    require(rs::RtsScriptSessionArchive::authoritativeHash(
        session, scripts, hash, error));
    return hash;
}

RtsLockstepFrame emptyFrame(
    sim::LockstepSessionId sessionId,
    std::uint64_t tick,
    sim::LockstepPeerId peerId,
    std::uint64_t sequence) {
    RtsLockstepFrame frame;
    frame.sessionId = sessionId;
    frame.tick = tick;
    frame.peerId = peerId;
    frame.frameSequence = sequence;
    return frame;
}

void testCombinedArchiveRestoresScriptFields() {
    ScriptFixture fixture;
    RtsGameSession session(16, 16);
    populateSession(session);
    rs::RtsScriptSession scripts(session, fixture.program, fixture.api);
    require(scripts.registerTeam(definition()));

    require(session.stepDetailed(0) == RtsStepResult::Advanced);
    require(scripts.processCompletedTick(0) == rs::RtsScriptTickResult::Processed);
    require(decisionCount(scripts) == 1);
    const auto expectedHash = combinedHash(session, scripts);

    realscript::runtime::RuntimeError error;
    const auto archive = rs::RtsScriptSessionArchive::encode(
        session, scripts, error);
    require(!archive.empty());

    require(session.stepDetailed(1) == RtsStepResult::Advanced);
    require(scripts.processCompletedTick(1) == rs::RtsScriptTickResult::Processed);
    require(decisionCount(scripts) == 2);

    require(rs::RtsScriptSessionArchive::decode(
        archive, session, scripts, error));
    require(session.simulation().nextExpectedTick() == 1);
    require(decisionCount(scripts) == 1);
    require(combinedHash(session, scripts) == expectedHash);
}

void testTimeMachineReplaysScriptState() {
    constexpr sim::LockstepSessionId sessionId = 0xB301u;
    ScriptFixture fixture;
    RtsGameSession session(16, 16);
    populateSession(session);
    rs::RtsScriptSession scripts(session, fixture.program, fixture.api);
    require(scripts.registerTeam(definition()));
    rs::RtsScriptLockstepSession lockstep(
        session, scripts, {sessionId, 0, 0, 1, 16, 1, 64});
    require(lockstep.registerPeer(
        {1, 10, 10, sim::LockstepPeerRole::Player, true}));
    require(lockstep.start() == RtsLockstepStartResult::Started);

    require(lockstep.receiveFrame(emptyFrame(sessionId, 0, 1, 1)) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(lockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);
    require(decisionCount(scripts) == 1);
    const auto tickZeroHash = combinedHash(session, scripts);

    require(lockstep.receiveFrame(emptyFrame(sessionId, 1, 1, 2)) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(lockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);
    require(decisionCount(scripts) == 2);
    const auto finalHash = combinedHash(session, scripts);

    require(lockstep.seekCompletedTick(0));
    require(session.simulation().nextExpectedTick() == 1);
    require(decisionCount(scripts) == 1);
    require(combinedHash(session, scripts) == tickZeroHash);
    require(lockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);
    require(decisionCount(scripts) == 2);
    require(combinedHash(session, scripts) == finalHash);
}

void testReconnectRebuildsObjectsInAnotherHeap() {
    constexpr sim::LockstepSessionId sessionId = 0xB302u;
    ScriptFixture sourceFixture;
    RtsGameSession sourceSession(16, 16);
    populateSession(sourceSession);
    rs::RtsScriptSession sourceScripts(
        sourceSession, sourceFixture.program, sourceFixture.api);
    require(sourceScripts.registerTeam(definition()));
    rs::RtsScriptLockstepSession sourceLockstep(
        sourceSession,
        sourceScripts,
        {sessionId, 0, 0, 1, 16, 1, 64});
    require(sourceLockstep.registerPeer(
        {1, 10, 10, sim::LockstepPeerRole::Player, true}));
    require(sourceLockstep.start() == RtsLockstepStartResult::Started);
    require(sourceLockstep.receiveFrame(emptyFrame(sessionId, 0, 1, 1)) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(sourceLockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);

    RtsReconnectSnapshot snapshot;
    require(sourceLockstep.makeReconnectSnapshot(snapshot));
    const auto expectedHash = combinedHash(sourceSession, sourceScripts);

    ScriptFixture targetFixture;
    RtsGameSession targetSession(16, 16);
    rs::RtsScriptSession targetScripts(
        targetSession, targetFixture.program, targetFixture.api);
    rs::RtsScriptLockstepSession targetLockstep(
        targetSession,
        targetScripts,
        {sessionId, 0, 0, 1, 16, 1, 64});
    require(targetLockstep.restoreReconnectSnapshot(std::move(snapshot)));
    require(targetLockstep.started());
    require(targetSession.simulation().nextExpectedTick() == 1);
    require(decisionCount(targetScripts) == 1);
    require(combinedHash(targetSession, targetScripts) == expectedHash);
}

} // namespace

int main() {
    testCombinedArchiveRestoresScriptFields();
    testTimeMachineReplaysScriptState();
    testReconnectRebuildsObjectsInAnotherHeap();
    std::cout << "RTSEngine RealScript Stage RS3 tests passed\n";
    return 0;
}
