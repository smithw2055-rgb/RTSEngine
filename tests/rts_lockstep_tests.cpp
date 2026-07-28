#include <RTSEngine/Rts/RtsLockstepArchive.h>

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace rts::gameplay;
namespace ecs = rts::ecs;
namespace sim = rts::sim;

void require(bool condition) {
    if (!condition) std::abort();
}

TickCommand moveCommand(
    std::uint64_t tick,
    std::uint32_t issuer,
    std::uint32_t sequence,
    ecs::Entity subject,
    GridPoint target) {
    TickCommand command;
    command.targetTick = tick;
    command.issuer = issuer;
    command.sequence = sequence;
    command.type = CommandType::Move;
    command.subject = subject;
    command.targetX = target.x;
    command.targetY = target.y;
    return command;
}

RtsLockstepFrame frame(
    sim::LockstepSessionId sessionId,
    std::uint64_t tick,
    sim::LockstepPeerId peerId,
    std::uint64_t frameSequence,
    std::vector<TickCommand> commands = {}) {
    RtsLockstepFrame value;
    value.sessionId = sessionId;
    value.tick = tick;
    value.peerId = peerId;
    value.frameSequence = frameSequence;
    value.commands = std::move(commands);
    return value;
}

void addTwoPlayers(RtsLockstepSession& lockstep) {
    require(lockstep.registerPeer(
        {1, 1, 1, sim::LockstepPeerRole::Player, true}));
    require(lockstep.registerPeer(
        {2, 2, 2, sim::LockstepPeerRole::Player, true}));
}

void addPlayerAndSpectator(RtsLockstepSession& lockstep) {
    require(lockstep.registerPeer(
        {1, 1, 1, sim::LockstepPeerRole::Player, true}));
    require(lockstep.registerPeer(
        {9, 0, 0, sim::LockstepPeerRole::Spectator, true}));
}

void testStrictLockstepAndFrameArchive() {
    constexpr sim::LockstepSessionId sessionId = 0xA001u;
    RtsGameSession session(12, 8);
    const auto first = session.createUnit({1, 1}, {1}, 1);
    require(first.valid());

    RtsLockstepSession lockstep(
        session,
        {sessionId, 0, 0, 1, 8, 1, 32});
    addTwoPlayers(lockstep);
    require(lockstep.start() == RtsLockstepStartResult::Started);

    const auto firstFrame = frame(sessionId, 0, 1, 1);
    require(lockstep.receiveFrame(firstFrame) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(lockstep.advanceOne() ==
            RtsLockstepAdvanceResult::WaitingForInput);

    const auto secondFrame = frame(sessionId, 0, 2, 1);
    const auto bytes = EncodeRtsLockstepFrame(secondFrame);
    require(!bytes.empty());
    RtsLockstepFrame decoded;
    require(DecodeRtsLockstepFrame(bytes, decoded));
    require(decoded.sessionId == secondFrame.sessionId);
    require(decoded.tick == secondFrame.tick);
    require(decoded.peerId == secondFrame.peerId);
    require(decoded.frameSequence == secondFrame.frameSequence);

    require(lockstep.receiveFrame(decoded) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(lockstep.receiveFrame(decoded) ==
            sim::LockstepFrameSubmitResult::Duplicate);
    auto conflict = decoded;
    conflict.frameSequence = 2;
    require(lockstep.receiveFrame(conflict) ==
            sim::LockstepFrameSubmitResult::Conflict);

    require(lockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);
    require(session.simulation().nextExpectedTick() == 1);
    require(lockstep.coordinator().confirmedThrough() == 1);
}

void testInputDelayAndLocalSequencing() {
    constexpr sim::LockstepSessionId sessionId = 0xA002u;
    RtsGameSession session(12, 8);
    const auto unit = session.createUnit({1, 1}, {1}, 1);
    require(unit.valid());

    RtsLockstepSession lockstep(
        session,
        {sessionId, 2, 0, 1, 8, 1, 32});
    addTwoPlayers(lockstep);
    require(lockstep.start() == RtsLockstepStartResult::Started);
    require(lockstep.coordinator().confirmedThrough() == 2);

    TickCommand intent;
    intent.type = CommandType::Move;
    intent.subject = unit;
    intent.targetX = 5;
    intent.targetY = 1;
    RtsLockstepFrame local;
    require(lockstep.buildLocalFrame(1, {intent}, local));
    require(local.tick == 2);
    require(local.peerId == 1);
    require(local.frameSequence == 1);
    require(local.commands.size() == 1);
    require(local.commands[0].targetTick == 2);
    require(local.commands[0].issuer == 1);
    require(local.commands[0].sequence == 1);
    require(lockstep.receiveFrame(local) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(lockstep.receiveFrame(frame(sessionId, 2, 2, 1)) ==
            sim::LockstepFrameSubmitResult::Accepted);

    require(lockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);
    require(lockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);
    require(lockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);
    require(session.simulation().nextExpectedTick() == 3);
}

struct PredictionFixture final {
    RtsGameSession session{12, 8};
    ecs::Entity teamOne{};
    ecs::Entity teamTwo{};

    PredictionFixture() {
        teamOne = session.createUnit({1, 2}, {1}, 1);
        teamTwo = session.createUnit({8, 2}, {1}, 2);
        require(teamOne.valid() && teamTwo.valid());
    }
};

void testLateInputRollbackMatchesTimelyInput() {
    constexpr sim::LockstepSessionId sessionId = 0xA003u;
    PredictionFixture late;
    PredictionFixture timely;

    const RtsLockstepConfig config{
        sessionId, 0, 2, 1, 16, 1, 32};
    RtsLockstepSession lateLockstep(late.session, config);
    RtsLockstepSession timelyLockstep(timely.session, config);
    addTwoPlayers(lateLockstep);
    addTwoPlayers(timelyLockstep);
    require(lateLockstep.start() == RtsLockstepStartResult::Started);
    require(timelyLockstep.start() == RtsLockstepStartResult::Started);

    require(lateLockstep.receiveFrame(frame(sessionId, 0, 1, 1)) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(lateLockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);
    require(lateLockstep.coordinator().predictionDepth() == 1);
    require(lateLockstep.receiveFrame(frame(sessionId, 1, 1, 2)) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(lateLockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);

    const auto lateMove = moveCommand(
        0, 2, 1, late.teamTwo, {4, 2});
    require(lateLockstep.receiveFrame(
                frame(sessionId, 0, 2, 1, {lateMove})) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(lateLockstep.coordinator().rollbackRequired());

    const auto timelyMove = moveCommand(
        0, 2, 1, timely.teamTwo, {4, 2});
    require(timelyLockstep.receiveFrame(frame(sessionId, 0, 1, 1)) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(timelyLockstep.receiveFrame(
                frame(sessionId, 0, 2, 1, {timelyMove})) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(timelyLockstep.advanceOne() ==
            RtsLockstepAdvanceResult::Advanced);
    require(timelyLockstep.receiveFrame(frame(sessionId, 1, 1, 2)) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(timelyLockstep.receiveFrame(frame(sessionId, 1, 2, 2)) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(timelyLockstep.advanceOne() ==
            RtsLockstepAdvanceResult::Advanced);
    require(timelyLockstep.advanceOne() ==
            RtsLockstepAdvanceResult::Advanced);

    require(lateLockstep.advanceOne() ==
            RtsLockstepAdvanceResult::AdvancedAfterRollback);
    require(!lateLockstep.coordinator().rollbackRequired());
    require(lateLockstep.lastRollback().requestedTick == 0);
    require(lateLockstep.lastRollback().restoredResumeTick == 0);
    require(lateLockstep.lastRollback().replayedTicks == 2);
    require(late.session.simulation().nextExpectedTick() == 3);
    require(timely.session.simulation().nextExpectedTick() == 3);
    require(RtsGameSessionArchive::authoritativeHash(late.session) ==
            RtsGameSessionArchive::authoritativeHash(timely.session));
}

void testTimeMachineReplay() {
    constexpr sim::LockstepSessionId sessionId = 0xA004u;
    RtsGameSession session(8, 8);
    require(session.createUnit({1, 1}, {1}, 1).valid());
    RtsLockstepSession lockstep(
        session,
        {sessionId, 0, 0, 1, 16, 1, 16});
    require(lockstep.registerPeer(
        {1, 1, 1, sim::LockstepPeerRole::Player, true}));
    require(lockstep.start() == RtsLockstepStartResult::Started);

    for (std::uint64_t tick = 0; tick < 3; ++tick) {
        require(lockstep.receiveFrame(frame(sessionId, tick, 1, tick + 1)) ==
                sim::LockstepFrameSubmitResult::Accepted);
        require(lockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);
    }
    const auto finalHash = RtsGameSessionArchive::authoritativeHash(session);
    const auto* tickZeroHash = lockstep.desync().localHash(0);
    require(tickZeroHash != nullptr);
    const auto expectedTickZeroHash = tickZeroHash->worldHash;

    require(lockstep.seekCompletedTick(0));
    require(session.simulation().nextExpectedTick() == 1);
    require(RtsGameSessionArchive::authoritativeHash(session) ==
            expectedTickZeroHash);
    require(lockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);
    require(lockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);
    require(RtsGameSessionArchive::authoritativeHash(session) == finalHash);
}

void testDesyncDiagnostics() {
    constexpr sim::LockstepSessionId sessionId = 0xA005u;
    RtsGameSession session(8, 8);
    require(session.createUnit({1, 1}, {1}, 1).valid());
    RtsLockstepSession lockstep(
        session,
        {sessionId, 0, 0, 1, 8, 1, 16});
    addTwoPlayers(lockstep);
    require(lockstep.start() == RtsLockstepStartResult::Started);

    for (std::uint64_t tick = 0; tick < 2; ++tick) {
        require(lockstep.receiveFrame(frame(sessionId, tick, 1, tick + 1)) ==
                sim::LockstepFrameSubmitResult::Accepted);
        require(lockstep.receiveFrame(frame(sessionId, tick, 2, tick + 1)) ==
                sim::LockstepFrameSubmitResult::Accepted);
        require(lockstep.advanceOne() == RtsLockstepAdvanceResult::Advanced);
    }

    sim::StateHashReport matching;
    require(lockstep.makeHashReport(2, 0, matching));
    const auto encoded = EncodeRtsStateHashReport(matching);
    require(!encoded.empty());
    sim::StateHashReport decoded;
    require(DecodeRtsStateHashReport(encoded, decoded));
    require(lockstep.receiveHashReport(decoded) ==
            sim::HashReportSubmitResult::Match);

    sim::StateHashReport mismatch;
    require(lockstep.makeHashReport(2, 1, mismatch));
    mismatch.authoritativeHash ^= 0x55u;
    require(lockstep.receiveHashReport(mismatch) ==
            sim::HashReportSubmitResult::Mismatch);
    require(lockstep.desync().incidents().size() == 1);
    const auto& incident = lockstep.desync().incidents().front();
    require(incident.peerId == 2);
    require(incident.tick == 1);
    require(incident.hasPreviousMatch);
    require(incident.previousMatchingTick == 0);
}

void testReconnectAndSpectatorContinuation() {
    constexpr sim::LockstepSessionId sessionId = 0xA006u;
    RtsGameSession source(10, 8);
    require(source.createUnit({1, 1}, {1}, 1).valid());
    const RtsLockstepConfig config{
        sessionId, 0, 0, 1, 8, 1, 16};
    RtsLockstepSession sourceLockstep(source, config);
    addPlayerAndSpectator(sourceLockstep);
    require(sourceLockstep.start() == RtsLockstepStartResult::Started);
    require(sourceLockstep.receiveFrame(frame(sessionId, 0, 1, 1)) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(sourceLockstep.advanceOne() ==
            RtsLockstepAdvanceResult::Advanced);

    RtsReconnectSnapshot snapshot;
    require(sourceLockstep.makeReconnectSnapshot(snapshot));
    const auto bytes = EncodeRtsReconnectSnapshot(snapshot);
    require(!bytes.empty());
    RtsReconnectSnapshot decoded;
    require(DecodeRtsReconnectSnapshot(bytes, decoded));

    RtsGameSession restored(10, 8);
    RtsLockstepSession restoredLockstep(restored, config);
    require(restoredLockstep.restoreReconnectSnapshot(std::move(decoded)));
    require(restored.simulation().nextExpectedTick() == 1);
    require(restoredLockstep.coordinator().peers().size() == 2);
    require(RtsGameSessionArchive::authoritativeHash(source) ==
            RtsGameSessionArchive::authoritativeHash(restored));

    const auto next = frame(sessionId, 1, 1, 2);
    require(sourceLockstep.receiveFrame(next) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(restoredLockstep.receiveFrame(next) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(sourceLockstep.advanceOne() ==
            RtsLockstepAdvanceResult::Advanced);
    require(restoredLockstep.advanceOne() ==
            RtsLockstepAdvanceResult::Advanced);
    require(RtsGameSessionArchive::authoritativeHash(source) ==
            RtsGameSessionArchive::authoritativeHash(restored));

    TickCommand intent;
    intent.type = CommandType::Stop;
    intent.subject = source.simulation().snapshot().entities.front().entity;
    RtsLockstepFrame sourceLocal;
    RtsLockstepFrame restoredLocal;
    require(sourceLockstep.buildLocalFrame(1, {intent}, sourceLocal));
    intent.subject = restored.simulation().snapshot().entities.front().entity;
    require(restoredLockstep.buildLocalFrame(1, {intent}, restoredLocal));
    require(sourceLocal.tick == restoredLocal.tick);
    require(sourceLocal.frameSequence == restoredLocal.frameSequence);
    require(sourceLocal.commands.front().sequence ==
            restoredLocal.commands.front().sequence);
}

} // namespace

int main() {
    testStrictLockstepAndFrameArchive();
    testInputDelayAndLocalSequencing();
    testLateInputRollbackMatchesTimelyInput();
    testTimeMachineReplay();
    testDesyncDiagnostics();
    testReconnectAndSpectatorContinuation();
    std::cout << "RTS lockstep tests passed\n";
    return EXIT_SUCCESS;
}
