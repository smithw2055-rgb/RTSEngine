#include <RTSEngine/RtsScripting/RtsScriptWorldRuntime.h>

#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <utility>

namespace rts::gameplay::scripting {
namespace {

void fail(realscript::runtime::RuntimeError& error,
          std::string message) {
    error.code = realscript::runtime::ErrorCode::InvalidProgram;
    error.message = std::move(message);
    error.stackTrace.clear();
}

bool sameIdentity(
    const rts::scripting::ScriptProgramIdentity* first,
    const rts::scripting::ScriptProgramIdentity* second) noexcept {
    if (!first || !second) return false;
    return first->bundle == second->bundle &&
           first->bundlePayloadHash == second->bundlePayloadHash &&
           first->script.sdkCompatibilityVersion ==
               second->script.sdkCompatibilityVersion &&
           first->script.gameSdkPackageVersion ==
               second->script.gameSdkPackageVersion &&
           first->script.hostApiHash == second->script.hostApiHash &&
           first->script.programContentHash ==
               second->script.programContentHash;
}

} // namespace

bool RtsScriptWorldRuntime::valid() const noexcept {
    return teams_.valid() && entities_.valid() &&
           sameIdentity(teams_.programIdentity(), entities_.programIdentity());
}

RtsScriptWorldTickResult RtsScriptWorldRuntime::processCompletedTick(
    std::uint64_t completedTick) {
    if (!valid()) return RtsScriptWorldTickResult::InvalidRuntime;
    const auto teamResult = teams_.processCompletedTick(completedTick);
    if (teamResult == RtsScriptTickResult::InvalidRuntime) {
        return RtsScriptWorldTickResult::TeamScriptFailed;
    }
    if (teamResult == RtsScriptTickResult::TimelineMismatch) {
        return RtsScriptWorldTickResult::TimelineMismatch;
    }
    const auto entityResult = entities_.processCompletedTick(completedTick);
    if (entityResult == RtsEntityBehaviorTickResult::InvalidRuntime) {
        return RtsScriptWorldTickResult::EntityBehaviorFailed;
    }
    if (entityResult == RtsEntityBehaviorTickResult::TimelineMismatch) {
        return RtsScriptWorldTickResult::TimelineMismatch;
    }
    return teamResult == RtsScriptTickResult::NoCallbacks &&
           entityResult == RtsEntityBehaviorTickResult::NoCallbacks
        ? RtsScriptWorldTickResult::NoCallbacks
        : RtsScriptWorldTickResult::Processed;
}

bool RtsScriptWorldRuntime::authoritativeHash(
    std::uint64_t& output,
    realscript::runtime::RuntimeError& error) const {
    return RtsScriptWorldArchive::authoritativeHash(
        session_, teams_, entities_, output, error);
}

std::vector<std::uint8_t> RtsScriptWorldRuntime::encode(
    realscript::runtime::RuntimeError& error) const {
    return RtsScriptWorldArchive::encode(
        session_, teams_, entities_, error);
}

bool RtsScriptWorldRuntime::restore(
    const std::vector<std::uint8_t>& bytes,
    realscript::runtime::RuntimeError& error) {
    return RtsScriptWorldArchive::decode(
        bytes, session_, teams_, entities_, error);
}

bool RtsScriptWorldArchive::authoritativeHash(
    const RtsGameSession& session,
    const RtsScriptSession& teams,
    const RtsEntityBehaviorRuntime& entities,
    std::uint64_t& output,
    realscript::runtime::RuntimeError& error) {
    if (!sameIdentity(teams.programIdentity(), entities.programIdentity())) {
        fail(error, "Team and entity scripts use different programs");
        return false;
    }
    std::uint64_t teamHash = 0;
    std::uint64_t entityHash = 0;
    if (!RtsScriptSessionArchive::authoritativeHash(
            session, teams, teamHash, error) ||
        !entities.authoritativeHash(entityHash, error)) {
        return false;
    }
    foundation::CanonicalHash hash;
    hash.WriteU16(kVersion);
    hash.WriteU64(teamHash);
    hash.WriteU64(entityHash);
    output = hash.Value();
    return true;
}

std::vector<std::uint8_t> RtsScriptWorldArchive::encode(
    const RtsGameSession& session,
    const RtsScriptSession& teams,
    const RtsEntityBehaviorRuntime& entities,
    realscript::runtime::RuntimeError& error) {
    const auto teamBytes = RtsScriptSessionArchive::encode(
        session, teams, error);
    if (teamBytes.empty() || teamBytes.size() > kMaximumTeamArchiveBytes) {
        if (error.code == realscript::runtime::ErrorCode::None) {
            fail(error, "Team script archive exceeds the world archive limit");
        }
        return {};
    }
    const auto entityBytes = entities.encodeState(error);
    if (entityBytes.empty() || entityBytes.size() > kMaximumEntityStateBytes) {
        if (error.code == realscript::runtime::ErrorCode::None) {
            fail(error, "Entity behavior state exceeds the world archive limit");
        }
        return {};
    }
    std::uint64_t hash = 0;
    if (!authoritativeHash(session, teams, entities, hash, error)) return {};

    foundation::BinaryWriter writer;
    writer.writeU32(kMagic);
    writer.writeU16(kVersion);
    writer.writeU64(hash);
    writer.writeU32(static_cast<std::uint32_t>(teamBytes.size()));
    writer.writeBytes(teamBytes);
    writer.writeU32(static_cast<std::uint32_t>(entityBytes.size()));
    writer.writeBytes(entityBytes);
    return writer.take();
}

bool RtsScriptWorldArchive::decode(
    const std::vector<std::uint8_t>& bytes,
    RtsGameSession& session,
    RtsScriptSession& teams,
    RtsEntityBehaviorRuntime& entities,
    realscript::runtime::RuntimeError& error) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint64_t storedHash = 0;
    std::uint32_t teamSize = 0;
    std::uint32_t entitySize = 0;
    std::vector<std::uint8_t> teamBytes;
    std::vector<std::uint8_t> entityBytes;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        magic != kMagic || version != kVersion ||
        !reader.readU64(storedHash) || storedHash == 0 ||
        !reader.readU32(teamSize) || teamSize == 0 ||
        teamSize > kMaximumTeamArchiveBytes ||
        !reader.readBytes(
            teamSize, teamBytes, kMaximumTeamArchiveBytes) ||
        !reader.readU32(entitySize) || entitySize == 0 ||
        entitySize > kMaximumEntityStateBytes ||
        !reader.readBytes(
            entitySize, entityBytes, kMaximumEntityStateBytes) ||
        !reader.atEnd()) {
        fail(error, "invalid RTS script world archive");
        return false;
    }

    realscript::runtime::RuntimeError backupError;
    const auto previousTeam = RtsScriptSessionArchive::encode(
        session, teams, backupError);
    const auto previousEntities = entities.encodeState(backupError);
    if (previousTeam.empty() || previousEntities.empty()) {
        fail(error, "current RTS script world cannot be backed up");
        return false;
    }

    const auto rollback = [&]() {
        realscript::runtime::RuntimeError ignored;
        (void)RtsScriptSessionArchive::decode(
            previousTeam, session, teams, ignored);
        (void)entities.restoreEncodedState(previousEntities, ignored);
    };

    if (!RtsScriptSessionArchive::decode(
            teamBytes, session, teams, error) ||
        !entities.restoreEncodedState(entityBytes, error)) {
        rollback();
        return false;
    }

    std::uint64_t restoredHash = 0;
    if (!authoritativeHash(
            session, teams, entities, restoredHash, error) ||
        restoredHash != storedHash) {
        rollback();
        if (error.code == realscript::runtime::ErrorCode::None) {
            fail(error, "RTS script world authoritative hash mismatch");
        }
        return false;
    }
    return true;
}

} // namespace rts::gameplay::scripting
